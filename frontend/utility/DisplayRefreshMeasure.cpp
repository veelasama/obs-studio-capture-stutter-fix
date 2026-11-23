#include "DisplayRefreshMeasure.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3dkmthk.h>

#include <algorithm>
#include <cmath>

namespace {

struct Sample {
	double t;    // QPC, seconds
	double line; // scan line at t
	double k = 0.0; // vblank index
};

BOOL CALLBACK EnumMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM param)
{
	auto *targets = reinterpret_cast<std::vector<DisplayRefreshTarget> *>(param);
	MONITORINFOEXW info = {};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(monitor, &info))
		return TRUE;

	DisplayRefreshTarget target;
	target.device = info.szDevice;
	target.width = info.rcMonitor.right - info.rcMonitor.left;
	target.height = info.rcMonitor.bottom - info.rcMonitor.top;
	target.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

	DEVMODEW mode = {};
	mode.dmSize = sizeof(mode);
	if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode))
		target.nominalHz = (int)mode.dmDisplayFrequency;

	targets->push_back(target);
	return TRUE;
}

/* Least squares t = a + b*k + c*line.  Returns false if singular. */
bool Fit(const std::vector<Sample> &s, double &a, double &b, double &c, double &sigma, double &sigmaB)
{
	double m[3][3] = {}, v[3] = {};
	for (const Sample &p : s) {
		const double x[3] = {1.0, p.k, p.line};
		for (int i = 0; i < 3; i++) {
			v[i] += x[i] * p.t;
			for (int j = 0; j < 3; j++)
				m[i][j] += x[i] * x[j];
		}
	}

	/* Invert the 3x3 normal matrix. */
	const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
			   m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
			   m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	if (std::fabs(det) < 1e-300)
		return false;

	double inv[3][3];
	inv[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
	inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
	inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
	inv[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
	inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
	inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
	inv[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
	inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
	inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;

	double coef[3] = {};
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			coef[i] += inv[i][j] * v[j];
	a = coef[0];
	b = coef[1];
	c = coef[2];

	double ss = 0.0;
	for (const Sample &p : s) {
		const double r = p.t - (a + b * p.k + c * p.line);
		ss += r * r;
	}
	sigma = std::sqrt(ss / std::max<size_t>(s.size() - 3, 1));
	sigmaB = sigma * std::sqrt(std::max(inv[1][1], 0.0));
	return true;
}

} // namespace

std::vector<DisplayRefreshTarget> EnumerateRefreshTargets()
{
	std::vector<DisplayRefreshTarget> targets;
	EnumDisplayMonitors(nullptr, nullptr, EnumMonitorProc, reinterpret_cast<LPARAM>(&targets));
	std::stable_sort(targets.begin(), targets.end(),
			 [](const DisplayRefreshTarget &x, const DisplayRefreshTarget &y) { return x.primary > y.primary; });
	return targets;
}

DisplayRefreshResult MeasureDisplayRefresh(const std::wstring &device, double seconds, const std::function<bool()> &stop)
{
	DisplayRefreshResult result;

	HDC dc = CreateDCW(device.c_str(), device.c_str(), nullptr, nullptr);
	if (!dc) {
		result.error = "CreateDC failed";
		return result;
	}

	D3DKMT_OPENADAPTERFROMHDC open = {};
	open.hDc = dc;
	const NTSTATUS opened = D3DKMTOpenAdapterFromHdc(&open);
	DeleteDC(dc);
	if (opened != 0) {
		result.error = "D3DKMTOpenAdapterFromHdc failed";
		return result;
	}

	LARGE_INTEGER frequency;
	QueryPerformanceFrequency(&frequency);
	const double qpcToSeconds = 1.0 / (double)frequency.QuadPart;
	auto now = [&]() {
		LARGE_INTEGER value;
		QueryPerformanceCounter(&value);
		return (double)value.QuadPart * qpcToSeconds;
	};

	/* Keep the waiting thread responsive; the result only depends on the
	 * timestamps taken right after each vblank. */
	const int oldPriority = GetThreadPriority(GetCurrentThread());
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

	std::vector<Sample> samples;
	samples.reserve((size_t)(seconds * 260.0) + 16);
	const double start = now();
	while (now() - start < seconds) {
		if (stop && stop()) {
			result.error = "cancelled";
			break;
		}

		D3DKMT_WAITFORVERTICALBLANKEVENT wait = {};
		wait.hAdapter = open.hAdapter;
		wait.VidPnSourceId = open.VidPnSourceId;
		if (D3DKMTWaitForVerticalBlankEvent(&wait) != 0) {
			result.error = "D3DKMTWaitForVerticalBlankEvent failed";
			break;
		}

		/* The wake-up latency after the event is noisy; the scan line is
		 * not.  Take the first reading in the active area and let the fit
		 * remove the line position. */
		for (int spin = 0; spin < 20000; spin++) {
			D3DKMT_GETSCANLINE scan = {};
			scan.hAdapter = open.hAdapter;
			scan.VidPnSourceId = open.VidPnSourceId;
			const double before = now();
			if (D3DKMTGetScanLine(&scan) != 0)
				break;
			const double after = now();
			if (!scan.InVerticalBlank) {
				if (after - before < 50e-6)
					samples.push_back({(before + after) * 0.5, (double)scan.ScanLine});
				break;
			}
		}
	}

	SetThreadPriority(GetCurrentThread(), oldPriority);

	D3DKMT_CLOSEADAPTER close = {};
	close.hAdapter = open.hAdapter;
	D3DKMTCloseAdapter(&close);

	if (!result.error.empty() || samples.size() < 60) {
		if (result.error.empty())
			result.error = "too few vblank samples";
		return result;
	}

	/* Rough period from the median spacing, then number every vblank so
	 * that missed wake-ups do not bias the fit. */
	std::vector<double> gaps;
	for (size_t i = 1; i < samples.size(); i++)
		gaps.push_back(samples[i].t - samples[i - 1].t);
	std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
	double period = gaps[gaps.size() / 2];

	for (int pass = 0; pass < 3; pass++) {
		for (Sample &p : samples)
			p.k = std::round((p.t - samples[0].t) / period);

		double a, b, c, sigma, sigmaB;
		if (!Fit(samples, a, b, c, sigma, sigmaB)) {
			result.error = "singular fit";
			return result;
		}
		period = b;
		result.hz = 1.0 / b;
		result.uncertaintyPpm = sigmaB / b * 1e6;

		/* Drop wake-ups that were preempted between the scan line read and
		 * the timestamp. */
		const double limit = std::max(4.0 * sigma, 20e-6);
		std::vector<Sample> kept;
		for (const Sample &p : samples)
			if (std::fabs(p.t - (a + b * p.k + c * p.line)) <= limit)
				kept.push_back(p);
		if (kept.size() == samples.size() || kept.size() < 60)
			break;
		samples.swap(kept);
	}

	result.ok = true;
	result.samples = (uint32_t)samples.size();
	result.seconds = samples.back().t - samples.front().t;
	return result;
}

void BestFrameRateFraction(double fps, uint32_t maxTerm, uint32_t &num, uint32_t &den)
{
	/* Convergents h/k of the continued fraction, stopping before a term
	 * exceeds maxTerm; the last semiconvergent is checked as well. */
	uint64_t h0 = 0, h1 = 1, k0 = 1, k1 = 0;
	double x = fps;
	num = (uint32_t)std::lround(fps);
	den = 1;
	for (int i = 0; i < 64; i++) {
		const double integral = std::floor(x);
		const uint64_t a = (uint64_t)integral;
		const uint64_t h2 = a * h1 + h0, k2 = a * k1 + k0;
		if (h2 > maxTerm || k2 > maxTerm) {
			const uint64_t limit = std::min((maxTerm - h0) / std::max<uint64_t>(h1, 1),
							(maxTerm - k0) / std::max<uint64_t>(k1, 1));
			const uint64_t hs = limit * h1 + h0, ks = limit * k1 + k0;
			if (ks && std::fabs((double)hs / ks - fps) < std::fabs((double)num / den - fps)) {
				num = (uint32_t)hs;
				den = (uint32_t)ks;
			}
			break;
		}
		num = (uint32_t)h2;
		den = (uint32_t)k2;
		h0 = h1;
		h1 = h2;
		k0 = k1;
		k1 = k2;
		const double frac = x - integral;
		if (frac < 1e-12)
			break;
		x = 1.0 / frac;
	}
}

double SecondsBetweenCorrections(double displayHz, double outputFps, double extraPpm)
{
	const double mismatch = std::fabs(displayHz - outputFps) + displayHz * extraPpm * 1e-6;
	return mismatch > 0.0 ? 1.0 / mismatch : INFINITY;
}

bool DisplayLockedFrameRate(double displayHz, double outputFps, double &lockedFps)
{
	if (displayHz <= 0.0 || outputFps <= 0.0)
		return false;

	const double ratio = outputFps / displayHz;
	double bestError = 0.003;
	bool found = false;
	for (int q = 1; q <= 12; q++) {
		const int p = (int)std::lround(ratio * q);
		if (p < 1)
			continue;
		const double error = std::fabs((double)p / q / ratio - 1.0);
		if (error < bestError - 1e-12) {
			bestError = error;
			lockedFps = displayHz * p / q;
			found = true;
		}
	}
	return found;
}
