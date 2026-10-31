#pragma once

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

/* Background refresh measurement for "match display before each recording".
 *
 * Measures only while no output uses video (obs_video_active() is false), so
 * a recording or stream never shares the CPU with it: 10 s per display, then a
 * pause.  StartRecording() reads the latest result without waiting. */
class DisplayRefreshMatcher {
public:
	DisplayRefreshMatcher();
	~DisplayRefreshMatcher();

	/* An empty device measures every display in turn, so the one the game
	 * runs on is known when a recording starts. */
	void Configure(bool enabled, const std::wstring &device);

	/* Latest successful measurement of `device`. */
	bool Latest(const std::wstring &device, double &hz, double &uncertaintyPpm, uint64_t &ageMs) const;

private:
	struct Result {
		double hz = 0.0;
		double ppm = 0.0;
		uint64_t tick = 0;
	};

	void Run();

	std::thread thread;
	mutable std::mutex mutex;
	std::condition_variable wake;
	bool stopping = false;
	bool enabled = false;
	uint64_t generation = 0;
	std::wstring device;
	std::map<std::wstring, Result> results;
};
