#include "DisplayRefreshMatcher.hpp"
#include "DisplayRefreshMeasure.hpp"

#include <obs.h>
#include <util/base.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <vector>

namespace {
constexpr double kMeasureSeconds = 10.0;
constexpr auto kPause = std::chrono::seconds(60);
constexpr auto kRetry = std::chrono::seconds(2);
} // namespace

DisplayRefreshMatcher::DisplayRefreshMatcher() : thread(&DisplayRefreshMatcher::Run, this) {}

DisplayRefreshMatcher::~DisplayRefreshMatcher()
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		stopping = true;
	}
	wake.notify_all();
	thread.join();
}

void DisplayRefreshMatcher::Configure(bool enable, const std::wstring &target)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (enabled == enable && device == target)
			return;
		enabled = enable;
		device = target;
		generation++;
		results.clear();
	}
	wake.notify_all();
}

bool DisplayRefreshMatcher::Latest(const std::wstring &target, double &hz, double &uncertaintyPpm,
				   uint64_t &ageMs) const
{
	std::lock_guard<std::mutex> lock(mutex);
	auto found = results.find(target);
	if (found == results.end())
		return false;
	hz = found->second.hz;
	uncertaintyPpm = found->second.ppm;
	ageMs = GetTickCount64() - found->second.tick;
	return true;
}

void DisplayRefreshMatcher::Run()
{
	std::unique_lock<std::mutex> lock(mutex);
	for (;;) {
		wake.wait(lock, [this] { return stopping || enabled; });
		if (stopping)
			return;

		const uint64_t started = generation;
		const std::wstring configured = device;
		lock.unlock();

		std::vector<std::wstring> targets;
		if (!configured.empty()) {
			targets.push_back(configured);
		} else {
			for (const DisplayRefreshTarget &target : EnumerateRefreshTargets())
				targets.push_back(target.device);
		}

		auto stop = [this, started] {
			if (obs_video_active())
				return true;
			std::lock_guard<std::mutex> guard(mutex);
			return stopping || generation != started;
		};

		bool complete = !targets.empty();
		for (const std::wstring &target : targets) {
			if (stop()) {
				complete = false;
				break;
			}
			const DisplayRefreshResult result = MeasureDisplayRefresh(target, kMeasureSeconds, stop);
			if (!result.ok) {
				if (result.error != "cancelled")
					blog(LOG_WARNING, "[display-fps] display %ls measurement failed: %s", target.c_str(),
					     result.error.c_str());
				complete = false;
				continue;
			}

			std::lock_guard<std::mutex> guard(mutex);
			if (generation != started)
				break;
			results[target] = Result{result.hz, result.uncertaintyPpm, GetTickCount64()};
			blog(LOG_INFO, "[display-fps] display %ls: %.7f Hz (+/- %.3f ppm, %u samples)", target.c_str(),
			     result.hz, result.uncertaintyPpm, result.samples);
		}

		lock.lock();
		wake.wait_for(lock, complete ? kPause : kRetry,
			      [this, started] { return stopping || generation != started; });
	}
}
