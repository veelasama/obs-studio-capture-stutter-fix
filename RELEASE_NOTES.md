# OBS Studio 32.2.2 Capture Stutter Fix v1.2.0

For 60 FPS games with V-Sync only — see the
[README](https://github.com/veelasama/obs-studio-capture-stutter-fix#is-this-build-for-you)
for what this build does and does not fix.

> **Required before the first recording:** *Settings → Video → Match Display →
> Measure and Set* (fractional frame rate locked to your monitor). Without it the
> build still records, but keeps correcting single frames every few minutes.
> Follow the [Setup](https://github.com/veelasama/obs-studio-capture-stutter-fix#setup)
> steps in the README.

## Changes

- **Window Capture and Display Capture in *Windows 10 (1903 and up)* mode**
  now use the same frame queue as Game Capture.
- **Match Display** also detects the display of a Window Capture window.

> **Capture Method must not be *Automatic*.** Window Capture → **Windows 10
> (1903 and up)**; Display Capture → **DXGI Desktop Duplication** or
> **Windows 10 (1903 and up)**. *Automatic* picks BitBlt for games, and
> BitBlt is not fixed. See [Supported capture sources](https://github.com/veelasama/obs-studio-capture-stutter-fix#supported-capture-sources).

Unchanged from v1.1.0: Game Capture and DXGI Display Capture queues, no
audio/video drift, Match Display for recordings only, *Limit capture
framerate* off for games above 60 FPS.

## Install

Installs to `C:\Program Files\obs-studio-capture-stutter-fix`, next to the
official OBS, and over earlier versions. The installer is **not code-signed**,
so Windows SmartScreen may show an unknown-publisher warning; verify the file
against `SHA256SUMS.txt`.
