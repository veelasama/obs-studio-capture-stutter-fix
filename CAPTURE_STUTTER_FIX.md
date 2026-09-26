# Capture Stutter Fix — technical notes

This document describes the mechanism and the implementation. For the
symptom checklist, setup and frame-rate scenarios see [README.rst](README.rst).

## Mechanism

- A game with V-Sync presents at the display's refresh rate, which is derived
  from the graphics card's clock. OBS video ticks come from
  `QueryPerformanceCounter`. On the reference PC the display ran at
  60.00283 Hz against QPC (+47 ppm); two monitors on the same card showed the
  same offset. The nominal mode (pixel clock 241.7 MHz, 2720×1481 total) is
  60.0002 Hz, so almost all of the offset is crystal tolerance between the two
  clock domains.
- The phase between "game frame ready" and "OBS tick" therefore moves by one
  frame every 1 / (47 ppm × 60 Hz) ≈ 350 s.
- Stock Game Capture reads one shared texture that the hook overwrites on
  every present. While the ready moment and the tick are within the present
  and GPU-completion jitter (up to ~13 ms after present in the recorded
  traces), each tick randomly sees the new or the previous image. The result is
  a burst of repeat/skip pairs that lasts as long as the phases overlap.
  A lab source at 59.98 FPS reproduced bursts of 14–26 consecutive repeats
  every 200 s with stock OBS.
- Frame counts must balance: `input = output − repeats + drops + Δqueue`. With
  a constant clock difference, a bounded-latency pipeline has to drop or repeat
  one frame per 350 s, or accumulate 170 ms of delay per hour. Only matching
  the output rate to the display rate removes the correction.

## Components

| Area | Files | Behavior |
|---|---|---|
| Queue protocol | `libobs-d3d11/ready_queue_protocol.hpp`, `plugins/win-capture/graphics-hook/ready_queue_*` | Eight shared textures, producer fence per frame, one-way handoff back to the stock texture on stop. |
| Producers | `graphics-hook/ready_queue_{producer,d3d9,d3d10,d3d12,gl,vulkan}*` | D3D11 fence; D3D9/D3D10 event queries; D3D11On12; `WGL_NV_DX_interop`; imported Vulkan images with `VkFence`. |
| Consumer | `libobs-d3d11/ready_queue_consumer.hpp`, `ready_queue_lowlatency.hpp` | Selection policy below; runs once per OBS video tick. |
| DXGI Display Capture | `libobs-d3d11/ready_queue_dxgi_duplicator.hpp` | Separate producer thread and device; pointer-only updates ignored; same selection policy. |
| Capture limiter | `graphics-hook/graphics-hook.h` `frame_ready()` | Token bucket, see below. |
| Preview | `libobs-d3d11/d3d11-subsystem.cpp` | Swap-chain maximum frame latency 2. |
| Scheduling | `libobs/obs-video.c` | Graphics thread joins MMCSS `Playback`, `AVRT_PRIORITY_HIGH`. |
| Match Display | `frontend/utility/DisplayRefreshMeasure.*`, `DisplayRefreshMatcher.*`, `GameDisplay.*`, `frontend/widgets/OBSBasic_DisplayFPS.cpp`, `frontend/settings/OBSBasicSettings_DisplayFPS.cpp` | Measurement, background matcher, game display detection, apply on recording start, settings UI. |

Hook resource version is 1.8.10 so that the OBS hook updater replaces the
registered Vulkan layer.

## Selection policy (`lowlatency`, default)

- Only frames whose producer fence has completed are eligible.
- Near 1:1 the queue is FIFO: the next sequence number is shown; if it is not
  ready the previous image is repeated and the next frame is kept.
- For a faster source the position advances by the measured ratio of output
  period to input period (input period from published timestamps over 5–10 s;
  output period from tick timestamps over 20 s, snapped to a standard rate
  within 50 ppm). 120 → 60 gives 2,2,2…, 144 → 60 gives 2,3,2,3,2…
- Latency controller: over a 10 s window (5 s for fast sources) the minimum age
  of the shown frame is tracked. If it exceeds one input frame plus the worst
  observed not-ready age plus 3 ms, exactly one frame is dropped.
- If the queue is nearly full, the position jumps so that the producer never
  runs out of slots.

`bounded` (v1.0.1) primes three frames and removes frames older than 66.7 ms
only at six ready frames or for sources faster than 14 ms per frame. Its
latency grew by the full clock difference: 26 ms → 77 ms over 20 minutes in the
recorded Rogue Trader trace.

## Capture limiter

The stock `frame_ready()` rejected a frame when less than one interval had
passed since the last accepted frame and reset its phase after a long gap. A
late frame followed by a quick one lost the quick one, and a source slightly
slower than the interval alternated accept/reject (29.9 of 59.9 frames/s in a
jitter test). The token bucket accrues credit with elapsed time, capped at two
intervals, and accepts when one interval of credit is available: the same
average ceiling without the phase reset.

It still selects frames by time. For a source at exactly twice the interval
(120 FPS game, 60 FPS output, option enabled) timing jitter can alternate the
selected parity; in the lab that produced 385 cadence errors in 120 s against
37 with the option disabled. The README therefore recommends keeping the
option off above 60 FPS.

## Display refresh measurement

`MeasureDisplayRefresh` opens the adapter of the selected display
(`D3DKMTOpenAdapterFromHdc`), waits for each vertical blank
(`D3DKMTWaitForVerticalBlankEvent`), and records the QPC time together with
the current scan line (`D3DKMTGetScanLine`) at the first reading outside the
blanking interval. A least-squares fit `t = a + b·vblank + c·scanline` removes
the wake-up latency; outliers are rejected. 15 s give about ±0.03 ppm.

`DisplayLockedFrameRate` keeps the configured relation between output rate and
display: it looks for the closest ratio p/q with q ≤ 12 within 0.3 % (60 on
60.0028 → 1/1, 30 → 1/2, 60 on 120 → 1/2, 60 on 144 → 5/12).
`BestFrameRateFraction` converts the result to `FPSNum/FPSDen` (terms up to
10⁶) by continued fractions.

The background matcher measures all displays in turn (or the selected one) for
10 s each, then pauses 60 s, and only while `obs_video_active()` is false.
`OBSBasic::StartRecording` resolves the display (selected, or the monitor of
the Game Capture window, then the foreground window, then the primary
display), and resets video with the new rate when it differs by more than
0.2 ppm. The matched rate is kept in memory and is not written to the
profile by itself, so the configured base rate (for example 60) stays the
starting point.

Log lines:

```
[display-fps] display \\.\DISPLAY2: 60.0028326 Hz (+/- 0.058 ppm, 575 samples)
[display-fps] \\.\DISPLAY2 (game window) 60.0028326 Hz (...): output 60/1 -> 699033/11650
[ready-queue] consumer stop selections=... repeats=... age_drops=... latency_sheds=...
```

## Limits

- Windows Graphics Capture, Window Capture (BitBlt/WGC), browser sources and
  capture cards are unchanged.
- HDR paths are present but were validated in SDR only.
- The Vulkan producer is unchanged from v1.0.1 and was not re-run for v1.1.0.
- A frame-rate limiter, G-SYNC or FreeSync makes the game follow the CPU clock;
  Match Display must be off in that case.
