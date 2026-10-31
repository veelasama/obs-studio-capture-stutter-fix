Capture stutter fix for **60 FPS games with V-Sync**, now based on **OBS Studio 33.0.1** — the same fixes as v1.2.0 (OBS 32.2.2). [What it fixes and what not →](https://github.com/veelasama/obs-studio-capture-stutter-fix#is-this-build-for-you)

> [!IMPORTANT]
> **Before the first recording:** Settings → Video → Match Display → **Measure and Set**. Step-by-step: [Setup](https://github.com/veelasama/obs-studio-capture-stutter-fix#setup).

**Capture source settings:**
- Game Capture → mode **Capture specific window**, *Limit capture framerate* off
- Window Capture → Capture Method **Modern (WGC)**, not *Automatic* (BitBlt is not fixed)
- Display Capture → Capture Method **Legacy (DXGI)** or **Modern (WGC)**, not *Automatic*

**Updating** — uninstall the previous version first, then install this one. Your settings are kept.

> [!CAUTION]
> **Unsigned installer** — Windows SmartScreen will show "Unknown publisher". Verify the file with `SHA256SUMS.txt`.
