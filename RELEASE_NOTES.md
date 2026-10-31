# OBS Studio 33.0.1 Capture Stutter Fix v1.3.0

For 60 FPS games with V-Sync only — see the
[README](https://github.com/veelasama/obs-studio-capture-stutter-fix#is-this-build-for-you)
for what this build does and does not fix.

> **Required before the first recording:** *Settings → Video → Match Display →
> Measure and Set* (fractional frame rate locked to your monitor). Without it the
> build still records, but keeps correcting single frames every few minutes.
> Follow the [Setup](https://github.com/veelasama/obs-studio-capture-stutter-fix#setup)
> steps in the README.

## Changes

- **Based on OBS Studio 33.0.1** (previously 32.2.2). See the
  [OBS 33.0 release notes](https://github.com/obsproject/obs-studio/releases/tag/33.0.0)
  for what is new in OBS itself.
- The capture fixes are unchanged from v1.2.0: Game Capture, Display Capture
  and Window Capture queues, no audio/video drift, Match Display.

> **Capture Method must not be *Automatic*.** OBS 33 renamed the methods:
> Window Capture → **Modern (WGC)**; Display Capture → **Legacy (DXGI)** or
> **Modern (WGC)**. *Automatic* picks BitBlt for games, and BitBlt is not
> fixed. See [Supported capture sources](https://github.com/veelasama/obs-studio-capture-stutter-fix#supported-capture-sources).

## Install

Installs to `C:\Program Files\obs-studio-capture-stutter-fix`, next to the
official OBS, and over earlier versions; settings are kept. Because OBS 33
moved its bundled modules to a new folder, the installer removes the program
files of the previous version first; third-party plugins that were installed
into this fork's folder must be reinstalled. The installer is **not
code-signed**, so Windows SmartScreen may show an unknown-publisher
warning; verify the file against `SHA256SUMS.txt`.
