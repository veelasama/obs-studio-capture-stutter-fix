#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/* Measures a display's real refresh rate against the system clock
 * (QueryPerformanceCounter), which is the clock OBS video ticks on.
 *
 * A game synchronized to the display presents at that rate, not at the
 * nominal one.  When the OBS output rate matches it, the capture queue never
 * has to drop or repeat a frame to absorb the difference. */

struct DisplayRefreshTarget {
	std::wstring device; // e.g. \\.\DISPLAY1
	int width = 0;
	int height = 0;
	int nominalHz = 0;
	bool primary = false;
};

struct DisplayRefreshResult {
	bool ok = false;
	double hz = 0.0;            // measured refresh rate
	double uncertaintyPpm = 0.0; // 1-sigma of the fitted period
	double seconds = 0.0;
	uint32_t samples = 0;
	std::string error;
};

std::vector<DisplayRefreshTarget> EnumerateRefreshTargets();

/* Blocks for about `seconds`.  Safe to call from a worker thread; `stop` is
 * polled once per refresh and ends the measurement early. */
DisplayRefreshResult MeasureDisplayRefresh(const std::wstring &device, double seconds,
					   const std::function<bool()> &stop = {});

/* Closest num/den with both terms <= maxTerm (continued fractions). */
void BestFrameRateFraction(double fps, uint32_t maxTerm, uint32_t &num, uint32_t &den);

/* Output rate locked to the display: displayHz * p / q for the simple ratio
 * p/q (q <= 12) closest to outputFps / displayHz, e.g. 60 FPS on a 60.0028 Hz
 * display -> 60.0028, 30 FPS -> 30.0014, 60 FPS on 144 Hz -> 5/12.  Returns
 * false when no such ratio is within 0.3%. */
bool DisplayLockedFrameRate(double displayHz, double outputFps, double &lockedFps);

/* Seconds of recording between two single-frame corrections when the output
 * rate is `outputFps` and the display may drift by another `extraPpm`. */
double SecondsBetweenCorrections(double displayHz, double outputFps, double extraPpm);
