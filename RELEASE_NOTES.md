Capture stutter fix for **60 FPS games with V-Sync**, now based on **OBS Studio 33.0.1** — the same fixes as v1.2.0 (OBS 32.2.2). [What it fixes and what not →](https://github.com/veelasama/obs-studio-capture-stutter-fix#is-this-build-for-you)

> [!IMPORTANT]
> **Before the first recording:** Settings → Video → Match Display → **Measure and Set**. Step-by-step: [Setup](https://github.com/veelasama/obs-studio-capture-stutter-fix#setup).

**Capture Method** — don't leave it on *Automatic*:
- Window Capture → **Modern (WGC)**
- Display Capture → **Legacy (DXGI)** or **Modern (WGC)**
- BitBlt is not fixed

**Updating** — uninstall the previous version first, then install this one. Your settings are kept.

**Unsigned installer** — Windows SmartScreen will show "Unknown publisher". Verify the file with `SHA256SUMS.txt`.
