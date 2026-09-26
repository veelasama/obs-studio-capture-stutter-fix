# OBS Studio 32.2.2 Capture Stutter Fix v1.1.0

For one specific problem: a **60 FPS game with V-Sync** that is smooth on the
monitor, while the OBS recording or preview periodically falls into bursts of
repeated and skipped frames even though OBS reports no rendering or encoding
lag. It does not address encoder overload, game frame drops, streaming network
problems or unlocked frame rates. Read the checklist in the
[README](https://github.com/veelasama/obs-studio-capture-stutter-fix#is-this-build-for-you)
before installing.

## Changes since v1.0.1

- **No audio/video drift.** v1.0.1 absorbed the difference between the display
  clock and the OBS clock by accumulating delay (about 25 → 80 ms behind the
  audio within 20 minutes of recording, then a two-frame drop). The new queue
  keeps latency at the minimum the GPU needs and corrects with single frames.
- **Match Display** (Settings → Video): measures the display's real refresh
  rate and locks the OBS frame rate to it, optionally before every recording.
  With V-Sync this removes the remaining corrections.
- **Capture limiter rewritten.** *Limit capture framerate* no longer drops to
  ~30 FPS and no longer loses frames after a game hitch.
- **Faster games.** 120 FPS into a 60 FPS recording is taken frame by frame in
  an even 2:1 cadence (959 → 37 cadence errors in the lab test). Keep *Limit
  capture framerate* off for games above 60 FPS.
- DXGI Display Capture uses the same low-latency selection.

v1.0.1 removed the long bursts too, but because of the drift above it is no
longer recommended.

## Install

Run `OBS-Studio-32.2.2-Capture-Stutter-Fix-v1.1.0-Windows-x64-Installer.exe`.
It installs to `C:\Program Files\obs-studio-capture-stutter-fix`, next to the
official OBS, and over v1.0.1 if present. The installer is not code-signed;
compare its hash with `SHA256SUMS.txt`.

Then follow **Setup** in the README: V-Sync on in the game, *Match Display:
Auto*, *Re-check automatically before each recording*. Use the matched rate for
recordings only; select 60 for streaming.
