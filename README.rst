OBS Studio Capture Stutter Fix
==============================

An independent Windows fork of `OBS Studio 32.2.2
<https://github.com/obsproject/obs-studio/releases/tag/32.2.2>`_ that removes
one specific, periodic capture stutter. It is not an official OBS Project
release and is not a general performance fix.

.. image:: https://github.com/veelasama/obs-studio-capture-stutter-fix/actions/workflows/capture-stutter-windows.yaml/badge.svg
   :alt: Windows build status
   :target: https://github.com/veelasama/obs-studio-capture-stutter-fix/actions/workflows/capture-stutter-windows.yaml

`Download the installer
<https://github.com/veelasama/obs-studio-capture-stutter-fix/releases/latest>`_ ·
`Technical notes <CAPTURE_STUTTER_FIX.md>`_ ·
`Feedback <https://github.com/veelasama/obs-studio-capture-stutter-fix/discussions/2>`_

.. contents::
   :local:
   :depth: 1

Is this build for you?
----------------------

The problem this fork addresses is rare. It appears only when **all** of the
following are true:

1. The game runs at **60 FPS with V-Sync on**, on a fixed-refresh display
   (G-SYNC / FreeSync not active for that game).
2. The game is smooth on the monitor. Its own frame counter stays at 60.
3. OBS records at 60 FPS and **View → Stats** shows no frames missed due to
   rendering lag and no frames skipped due to encoding lag.
4. The recording or the OBS preview nevertheless shows periodic episodes of
   repeated and skipped frames. Motion looks like 30–40 FPS for a few seconds
   to a minute, recovers on its own, and returns several minutes later.
5. The preview can show it even while nothing is being recorded.

If any of these points does not match, this build is unlikely to help. In
particular, it does **not** fix:

- rendering lag or encoder overload reported by OBS (lower the output
  resolution, change the encoder preset, reduce the game's GPU load);
- frame drops of the game itself — if the game stutters on the monitor, the
  recording shows the same stutter;
- dropped frames while streaming (network);
- games with an unlocked or fluctuating frame rate;
- stutter that appears only in a particular video player.

Please check the list above before reporting a problem.

What causes it
--------------

A game with V-Sync presents frames at the display's real refresh rate. That
rate is derived from the graphics card's clock, while OBS produces video
ticks from the CPU's clock. On the reference PC the display measured
60.00283 Hz against the CPU clock — 47 ppm faster than the 60.000 FPS OBS
records at. Both monitors attached to the same graphics card showed the same
offset. This is normal crystal tolerance, and it differs from PC to PC, which
is why the problem affects some systems and not others.

Because of that difference, the moment a game frame becomes ready slides
slowly relative to the moment OBS takes a frame: one full frame every
~6 minutes at 47 ppm. Stock OBS copies whatever image the game delivered
most recently. While the two moments are within the timing jitter of each
other, every OBS tick randomly gets either the new or the previous frame, and
the result is a burst of repeats and skips that lasts as long as the phases
overlap — seconds to minutes. The OBS preview had a related weakness: it gave
up a present when the display compositor was a fraction of a millisecond late.

What this fork changes
----------------------

- **Queue of completed frames.** Game Capture producers publish finished GPU
  frames into an eight-slot shared queue with fences. OBS takes them in order
  by sequence number, not by timestamp, so timing jitter no longer decides
  which frame is shown. Faster games (for example 120 FPS into a 60 FPS
  recording) are decimated by the measured frame ratio, giving an even 2:1
  cadence.
- **Low, stable latency.** The queue keeps only as much delay as the GPU needs
  to finish the copy. When a clock difference accumulates a spare frame, one
  single frame is dropped. The picture never drifts behind the audio.
- **Match Display** (Settings → Video). Measures the display's real refresh
  rate against the system clock and sets the OBS frame rate to it (for
  example 60.00283 FPS). With V-Sync, the game and OBS then run on the same
  clock and no correction is needed at all.
- **Game Capture limiter.** The **Limit capture framerate** limiter was
  rewritten as a token bucket. It no longer collapses to ~30 FPS when a game
  runs slightly below the output rate and no longer discards the frame a game
  presents right after a late one.
- **Preview and scheduling.** Preview swap-chain latency is two frames, and
  the OBS graphics thread is registered with MMCSS (``Playback``, high).
- **Display Capture (DXGI).** Desktop Duplication uses its own queue with the
  same frame selection. Windows Graphics Capture is unchanged.

The Game Capture queue covers Direct3D 9, 10, 11, 12, OpenGL and Vulkan, with
64-bit and 32-bit hooks. If the queue cannot be created, OBS falls back to its
stock capture path.

Setup
-----

1. Install from the `latest release
   <https://github.com/veelasama/obs-studio-capture-stutter-fix/releases/latest>`_
   and start **OBS Studio Capture Stutter Fix**. It installs to
   ``C:\Program Files\obs-studio-capture-stutter-fix`` next to an official OBS
   installation and uses its own settings. The installer is not code-signed;
   compare the SHA-256 value with ``SHA256SUMS.txt`` from the release.
2. In the game: **V-Sync on**, 60 FPS, and G-SYNC / FreeSync off for this game.
3. In OBS, **Settings → Video**:

   - *Common FPS Values*: **60** (the starting point).
   - *Match Display*: **Auto (display with the game)**. The text below the
     list shows which display the Game Capture window is on; you can also pick
     a display manually.
   - Enable **Re-check automatically before each recording** and click
     **Apply**. Displays are measured in the background while nothing records
     or streams; the frame rate is set from the latest measurement when a
     recording starts, without delaying it. The FPS fields are locked while
     this option is on.
   - Alternatively, click **Measure and Set** once to store a fixed value.

4. Add a **Game Capture** source in *Capture specific window* mode and leave
   *Limit capture framerate* off.
5. Record. The log contains a line such as
   ``[display-fps] \\.\DISPLAY2 (game window) 60.0028326 Hz ...: output 60/1 -> 699033/11650``.

The recording then has a frame rate such as 60.0028 FPS; the file is otherwise
an ordinary OBS recording. **Use it for recordings only**: streaming
services expect a standard frame rate. For streaming, turn the automatic
option off and select 60. While a stream, replay buffer or virtual camera is
already running, the automatic mode does not change the frame rate.

Re-measure (or keep the automatic option on) after changing the display mode,
cable, display or graphics card.

Frame rate scenarios
--------------------

The recording is assumed to be 60 FPS. What matters is which clock the game
follows: V-Sync ties it to the display; an FPS limiter, G-SYNC or FreeSync
ties it to the CPU clock that OBS already uses.

.. list-table::
   :header-rows: 1
   :widths: 24 30 46

   * - Game
     - Settings
     - Result
   * - **60 FPS, V-Sync, 60 Hz display** (the target case)
     - Match Display: Auto
     - The periodic episodes disappear. With the matched rate, a single-frame
       correction is expected at most once in several hours. Without Match
       Display the queue still removes the episodes but drops one frame about
       every 6 minutes.
   * - 120 FPS, V-Sync, 120 Hz display
     - Match Display: Auto (locks to exactly half the display rate);
       *Limit capture framerate* **off**
     - Even 2:1 cadence, no corrections, lower game latency than 60 Hz. The
       smoothest configuration if the hardware allows it.
   * - 144 FPS, V-Sync, 144 Hz display
     - Match Display: Auto; *Limit capture framerate* **off**
     - No drift corrections, but 144 → 60 is inherently uneven (2-3-2-3
       frames). For an even cadence record at 72 FPS or run the display at
       120 Hz.
   * - Any FPS with an FPS limiter (in-game, RTSS, NVIDIA Max Frame Rate)
       and V-Sync off, or with G-SYNC / FreeSync
     - Standard 60 FPS; Match Display **off**
     - The game already runs on the same clock as OBS, so this problem does
       not occur and this build offers little over stock OBS. Matching the
       display here would create an unnecessary drift.
   * - Unlocked or fluctuating FPS
     - —
     - The unevenness comes from the game; no capture change can remove it.
   * - Game below 60 FPS
     - —
     - Repeated frames are the game's own and appear on the monitor too.
   * - 59.94 Hz display
     - Match Display: Auto
     - Locks to the display's real rate near 59.94.

*Limit capture framerate*: with a 60 FPS game it is harmless in this build.
For games running above the recording frame rate keep it **off**. The limiter
picks frames by time, so with an exact 2:1 ratio timing jitter can again
decide which frame is taken; with the option off the queue picks every second
frame by sequence number (in the 120 FPS test: 385 cadence errors with the
option on, 37 with it off). In stock OBS and in v1.0.1 keep it off in all
cases, because the old limiter can drop to ~30 FPS.

Previous release (v1.0.1)
-------------------------

v1.0.1 already removed the long periodic episodes, but its queue absorbed the
clock difference by accumulating delay: in a measured 20-minute recording the
picture fell behind the audio from about 25 ms to 80 ms, and once the queue
filled up it dropped two frames at once. It also kept the old limiter and
could produce short bursts of uneven frames with games above 60 FPS. v1.1.0
keeps the latency constant and adds Match Display. The v1.1.0 installer
installs over v1.0.1 in the same folder.

Validation
----------

Measured on Windows 11, GeForce RTX 5070 Ti, 2560×1440, with a test source
that replays real Rogue Trader present timings and draws a frame number into
every frame. The number was decoded from every frame of each recording. 240 s
per run, v1.0.1 against v1.1.0:

.. list-table::
   :header-rows: 1

   * - Scenario
     - v1.0.1
     - v1.1.0
   * - Rogue Trader timing, clock drift ×10
     - 5 corrections (one skipped two frames); latency varies over 58 ms
     - 6 single-frame corrections; latency varies over 33 ms
   * - 120 FPS source into 60 FPS
     - 959 cadence errors, 58 seconds with bursts
     - 37 cadence errors, 2 seconds with bursts
   * - Game hitch (late frame, then quick frame) every 3 s
     - 72 repeats + 79 skips
     - 0 repeats
   * - 120 FPS source, *Limit capture framerate* on (120 s)
     - —
     - 385 cadence errors — keep the option off above 60 FPS
   * - *Limit capture framerate* on, 59.94 FPS source
     - 578 repeats + 564 skips, 138 seconds with bursts
     - 14 repeats (the unavoidable number for 59.94 → 60)

With Match Display active, a recording started from the automatic mode had
exactly 699033/11650 = 60.00283 FPS and the OBS graphics thread held
16.6659 ms per frame. In Rogue Trader (Direct3D 11) a 7.5-minute gameplay
recording contained a single one-frame correction, which is the case Match
Display removes. Satisfactory (Direct3D 12) showed a clear improvement.

Not verified: HDR, Vulkan in this release (the Vulkan producer is unchanged
from v1.0.1), Windows Graphics Capture, Linux and macOS.

Environment variables
---------------------

For isolating problems; none are needed in normal use.

==============================  =================================================
``OBS_READY_QUEUE=0``           Disable the Game Capture queue (stock path).
``OBS_DXGI_READY_QUEUE=0``      Disable the DXGI Display Capture queue.
``OBS_READY_QUEUE_POLICY=``     ``lowlatency`` (default), ``bounded`` (v1.0.1
                                behavior), ``latest`` (newest frame).
==============================  =================================================

Build it yourself
-----------------

Every release is built by GitHub Actions from this repository
(``.github/workflows/capture-stutter-windows.yaml``); the installer definition
is ``.github/installer/capture-stutter-fix.nsi``.

A local build needs Visual Studio 2026 with *Desktop development with C++*,
the Windows 11 SDK 10.0.26100, CMake 4.2 or later and Git::

   git clone --recurse-submodules https://github.com/veelasama/obs-studio-capture-stutter-fix.git
   cd obs-studio-capture-stutter-fix
   cmake --preset windows-x64
   cmake --build --preset windows-x64 --config RelWithDebInfo
   cmake --install build_x64 --prefix build_x64/install --config RelWithDebInfo

The OBS dependency bundles, including Qt, are downloaded by CMake during
configuration. The CI script ``.github/scripts/Build-Windows.ps1`` performs
the same steps.

Support
-------

This is a focused experimental fork, not a separately supported OBS
distribution. The `feedback discussion
<https://github.com/veelasama/obs-studio-capture-stutter-fix/discussions/2>`_
asks only whether the build helped with the symptom described above. For
anything else, use the official OBS Project support channels.

Credits are listed in `FORK_AUTHORS.md <FORK_AUTHORS.md>`_. OBS Studio remains
the work of the OBS Project contributors. All changes are distributed under
GPL-2.0-or-later.
