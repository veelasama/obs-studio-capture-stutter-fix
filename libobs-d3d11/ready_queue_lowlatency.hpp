// Ready-queue "lowlatency" selection policy, GPL-2.0-or-later.
// Standalone so it can be replayed against recorded traces without D3D.
//
// Sequence-space selection with a slow latency controller.
//
// Near 1:1 input it is FIFO: timestamp jitter never decides which frame is
// shown, and an underflow repeats while keeping the next frame.  A faster
// input advances a fractional position by the measured input frames per
// output tick, so 120/144 -> 60 decimation cadence follows sequence numbers
// instead of where noisy timestamps fall relative to an age threshold.
//
// Latency does not accumulate: when the minimum selected age over a window
// proves one whole input frame of spare latency above the worst observed
// GPU-completion age, exactly one frame is shed.  With a constant clock
// mismatch this is the unavoidable correction (N = O - R + D + dQ), spent as
// single frames instead of as a growing A/V offset.
#pragma once
#include <stdint.h>
#include <algorithm>
#include <cmath>

struct RQLowLatency {
    int64_t frequency = 0;
    double pos = -1.0;            // Fractional queue sequence to show; <0 until primed.
    double input_ticks = 0.0;     // Estimated input frame period, QPC ticks.
    double output_ticks = 0.0;    // Output (video tick) period, QPC ticks.
    double tick_estimate = 0.0;
    double not_ready_ticks = 0.0; // Worst recent age of a published, fence-incomplete frame.
    double window_min_age = 0.0;  // Minimum selected age in the current window, QPC ticks.
    int64_t window_start = 0, tick_anchor_qpc = 0;
    int64_t anchor_qpc[2] = {};
    uint64_t anchor_seq[2] = {};
    uint64_t last_selected = 0, sheds = 0, ticks = 0;

    void init(int64_t qpc_frequency)
    {
        frequency = qpc_frequency;
        input_ticks = output_ticks = frequency / 60.0;
        not_ready_ticks = frequency * 0.014;
    }

    // Once per output tick, before observe_pending/select.
    void begin_tick(int64_t now)
    {
        observe_tick(now);
        not_ready_ticks *= 0.9999994; // ~1% per 17 s at 60 Hz
    }

    // For every published slot whose producer fence is not complete yet.
    void observe_pending(int64_t now, int64_t submitted)
    {
        not_ready_ticks = std::min(std::max(not_ready_ticks, (double)(now - submitted)), frequency * 0.05);
    }

    // seq/submitted: fence-complete ready frames sorted by sequence.
    // Returns the index to show (older entries are dropped) or -1 to repeat.
    int select(const uint64_t *seq, const int64_t *submitted, unsigned count, unsigned capacity, int64_t now)
    {
        if (!count)
            return -1;
        const double freq = (double)frequency;
        const uint64_t oldest = seq[0], newest = seq[count - 1];
        const int64_t newest_qpc = submitted[count - 1];

        // Input period over a 5-10 s window of published sequences.
        if (!anchor_seq[0]) {
            anchor_seq[0] = anchor_seq[1] = newest;
            anchor_qpc[0] = anchor_qpc[1] = newest_qpc;
        } else if (newest_qpc - anchor_qpc[1] > frequency * 5) {
            anchor_seq[0] = anchor_seq[1];
            anchor_qpc[0] = anchor_qpc[1];
            anchor_seq[1] = newest;
            anchor_qpc[1] = newest_qpc;
        }
        if (newest > anchor_seq[0] + 30 && newest_qpc > anchor_qpc[0])
            input_ticks = (double)(newest_qpc - anchor_qpc[0]) / (double)(newest - anchor_seq[0]);

        const double rho = output_ticks / std::max(input_ticks, 1.0);
        const bool fast = rho > 1.005;
        const double step = fast ? rho : 1.0;

        if (pos < 0.0)
            pos = (double)newest;
        else
            pos += step;
        if (count + 1 >= capacity) // Never let the producer run out of slots.
            pos = std::max(pos, (double)seq[count - 2]);
        if ((uint64_t)pos < oldest) // Sequence gap: frames were never published.
            pos = (double)oldest + (pos - std::floor(pos));
        uint64_t target = (uint64_t)pos;
        if (target > newest) {
            if (fast && newest > last_selected) {
                pos = (double)newest; // Show what exists; keep the cadence origin.
                target = newest;
            } else {
                pos -= step; // FIFO underflow: repeat, keep the next frame.
                return -1;
            }
        }
        unsigned k = 0;
        while (k + 1 < count && seq[k + 1] <= target)
            ++k;

        const double age = (double)(now - submitted[k]);
        const int64_t window = frequency * (fast ? 5 : 10);
        if (!window_start) {
            window_start = now;
            window_min_age = age;
        }
        window_min_age = std::min(window_min_age, age);
        if (now - window_start >= window) {
            const double margin = freq * 0.003;
            const double spare = window_min_age - input_ticks - not_ready_ticks - margin;
            if (spare > 0.0 && k + 1 < count) {
                ++k;
                pos += 1.0;
                ++sheds;
            }
            window_start = now;
            window_min_age = 1e300;
        }
        last_selected = seq[k];
        return (int)k;
    }

private:
    // Output period from the video ticks themselves (the backend does not see
    // obs_video_info).  Long baseline, smoothed, then snapped to a common
    // integer or NTSC rate when it is within 50 ppm.
    void observe_tick(int64_t now)
    {
        const double freq = (double)frequency;
        if (!tick_anchor_qpc || now - tick_anchor_qpc > frequency * 20) {
            if (tick_anchor_qpc && ticks > 60) {
                const double measured = (double)(now - tick_anchor_qpc) / (double)ticks;
                tick_estimate = tick_estimate > 0.0 ? tick_estimate + (measured - tick_estimate) * 0.25 : measured;
                output_ticks = tick_estimate;
                const double fps = freq / tick_estimate;
                for (int n = 10; n <= 360; ++n) {
                    for (const double nominal : {(double)n, n * 1000.0 / 1001.0}) {
                        if (std::fabs(fps / nominal - 1.0) < 5e-5)
                            output_ticks = freq / nominal;
                    }
                }
            }
            tick_anchor_qpc = now;
            ticks = 0;
        }
        ++ticks;
    }
};
