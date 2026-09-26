# OBS Studio 32.2.2 Capture Stutter Fix v1.1.0

For 60 FPS games with V-Sync only — see the
[README](https://github.com/veelasama/obs-studio-capture-stutter-fix#is-this-build-for-you)
for what this build does and does not fix.

> **Required before the first recording:** configure *Settings → Video →
> Match Display* (fractional frame rate locked to your monitor). Without it the
> build still records, but keeps correcting single frames every few minutes.
> Follow the [Setup](https://github.com/veelasama/obs-studio-capture-stutter-fix#setup)
> steps in the README.

## Changes

- **No audio/video drift.** v1.0.1 let the picture fall behind the audio during
  long recordings. Latency now stays constant.
- **Match Display** (Settings → Video): locks the OBS frame rate to the
  monitor's real refresh rate, optionally before every recording. Recordings
  only; use 60 FPS for streaming.
- **Limit capture framerate** no longer drops to ~30 FPS or loses frames after
  a game hitch.
- **Games above 60 FPS** are captured in an even cadence. Keep *Limit capture
  framerate* off for them.

## Install

Installs to `C:\Program Files\obs-studio-capture-stutter-fix`, next to the
official OBS, and over v1.0.1. The installer is **not code-signed**, so Windows
SmartScreen may show an unknown-publisher warning; verify the file against
`SHA256SUMS.txt`.
