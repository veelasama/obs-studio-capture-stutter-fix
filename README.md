# OBS Studio Capture Stutter Fix

An independent Windows fork of [OBS Studio 32.2.2](https://github.com/obsproject/obs-studio/releases/tag/32.2.2)
that removes one specific, periodic capture stutter. It is not an official OBS
Project release and not a general performance fix.

[![Windows build status](https://github.com/veelasama/obs-studio-capture-stutter-fix/actions/workflows/capture-stutter-windows.yaml/badge.svg)](https://github.com/veelasama/obs-studio-capture-stutter-fix/actions/workflows/capture-stutter-windows.yaml)

**[Download the installer](https://github.com/veelasama/obs-studio-capture-stutter-fix/releases/latest)** ·
[Technical notes](CAPTURE_STUTTER_FIX.md) ·
[Feedback](https://github.com/veelasama/obs-studio-capture-stutter-fix/discussions/2)

## Is this build for you?

It helps only when **all** of these are true:

1. The game runs at **60 FPS with V-Sync on** (G-SYNC / FreeSync off for it).
2. The game itself is smooth on the monitor.
3. OBS **View → Stats** shows no rendering lag and no encoding lag.
4. Yet the recording or preview periodically looks like 30–40 FPS for a few
   seconds to a minute, recovers by itself, and comes back minutes later.

It does **not** fix OBS overload (rendering or encoding lag), stutter of the
game itself, network drops while streaming, or games with an unlocked or
fluctuating frame rate.

## Supported capture sources

| Source | Capture Method | Fixed |
|---|---|---|
| **Game Capture** | — | ✅ |
| **Display Capture** | *DXGI Desktop Duplication* | ✅ |
| **Display Capture** | *Windows 10 (1903 and up)* | ✅ since v1.2.0 |
| **Window Capture** | *Windows 10 (1903 and up)* | ✅ since v1.2.0 |
| **Window Capture** | *BitBlt (Windows 7 and up)* | ❌ |

> **Do not leave *Capture Method* on *Automatic*.** Select it explicitly:
> **Window Capture → *Windows 10 (1903 and up)***, **Display Capture →
> *DXGI Desktop Duplication* or *Windows 10 (1903 and up)***. In Window
> Capture *Automatic* picks BitBlt for almost every game, and BitBlt is not
> fixed.

In OBS 33 these methods are called *Modern (WGC)*, *Legacy (DXGI)* and
*Legacy (BitBlt)*.

## Setup

1. Install from the [latest release](https://github.com/veelasama/obs-studio-capture-stutter-fix/releases/latest)
   and start **OBS Studio Capture Stutter Fix**. It installs next to the
   official OBS and keeps its own settings.
2. In the game: **V-Sync on**, 60 FPS, G-SYNC / FreeSync off.
3. **Required — Settings → Video:** set *Common FPS Values* to **60**, choose
   *Match Display*: **Auto (display with the game)**, click **Measure and Set**
   (takes 15 s), then **Apply**.
4. Add **Game Capture** (*Capture specific window*), leave *Limit capture
   framerate* off. With **Window Capture** set *Capture Method* to
   **Windows 10 (1903 and up)**; with **Display Capture** to **DXGI Desktop
   Duplication** or **Windows 10 (1903 and up)** — not *Automatic*.
5. Record.

Match Display gives the recording a frame rate such as 60.0028 FPS.
**Use it for recordings only**; for streaming select 60 again.

<details>
<summary>Setup details</summary>

- **Installer.** Installs to `C:\Program Files\obs-studio-capture-stutter-fix`,
  over earlier versions of this fork. It is not code-signed, so SmartScreen
  may warn about an unknown publisher; compare the file with `SHA256SUMS.txt`
  from the release.
- **Why step 3 is required.** Without it the build still removes the long
  episodes, but drops a single frame every few minutes.
- **Which display.** The text under the *Match Display* list shows which
  display the captured window is on. You can also pick a display manually.
- **When to measure again.** After changing the display mode, cable, display
  or graphics card. The measured value stays stable otherwise.
- **Alternative: automatic mode.** Instead of the button, enable
  **Re-check automatically before each recording**. Displays are then measured
  in the background (about 10 s each) while nothing records or streams, and
  the frame rate is set from the latest measurement when a recording starts,
  without delaying it. The FPS fields are locked while this option is on; for
  streaming turn it off. If no measurement exists yet (recording started
  seconds after launching OBS), the rate already stored in the profile is used,
  so click **Measure and Set** once even when using this mode.
- **Other outputs.** While a stream, replay buffer or virtual camera is
  running, the frame rate is not changed.
- **Check it worked.** The log contains a line such as
  `[display-fps] \\.\DISPLAY2 (game window) 60.0028326 Hz ...: output 60/1 -> 699033/11650`.

</details>

<details>
<summary>Other frame rates (120 Hz, 144 Hz, FPS limiter, G-SYNC)</summary>

What matters is which clock the game follows: V-Sync ties it to the display;
an FPS limiter, G-SYNC or FreeSync ties it to the CPU clock that OBS already
uses. The recording is assumed to be 60 FPS.

| Game | Settings | Result |
|---|---|---|
| **60 FPS, V-Sync, 60 Hz** (target case) | Match Display: Auto | Episodes disappear; at most one single-frame correction in several hours. |
| 120 FPS, V-Sync, 120 Hz | Match Display: Auto; *Limit capture framerate* **off** | Even 2:1 cadence, no corrections. The smoothest option. |
| 144 FPS, V-Sync, 144 Hz | Match Display: Auto; *Limit capture framerate* **off** | No drift, but 144 → 60 is inherently uneven (2-3-2-3). Record at 72 FPS or use 120 Hz for an even cadence. |
| FPS limiter (in-game, RTSS, NVIDIA) with V-Sync off, or G-SYNC / FreeSync | Standard 60; Match Display **off** | The problem does not occur; this build adds little. |
| Unlocked or fluctuating FPS | — | The unevenness comes from the game. |
| Below 60 FPS | — | Repeats are the game's own and visible on the monitor too. |
| 59.94 Hz display | Match Display: Auto | Locks to the real rate near 59.94. |

*Limit capture framerate* is harmless with a 60 FPS game in this build. Above
60 FPS keep it **off**: it picks frames by time, so jitter can again decide
which frame is taken (385 cadence errors in a 120 FPS test with it on, 37 with
it off).

</details>

<details>
<summary>What causes the stutter and what this fork changes</summary>

A V-Sync game presents at the display's real refresh rate, which comes from
the graphics card's clock; OBS ticks from the CPU clock. On the reference PC
the display ran at 60.00283 Hz — 47 ppm faster than OBS's 60.000 FPS. The
moment a game frame becomes ready therefore slides past the moment OBS takes
a frame, one full frame every ~6 minutes. Stock OBS copies whatever image
arrived last; while the two moments are within the timing jitter, each tick
randomly gets the new or the previous frame — a burst of repeats and skips
lasting seconds to a minute.

Changes:

- **Queue of completed frames** for Game Capture (Direct3D 9/10/11/12, OpenGL,
  Vulkan), DXGI Display Capture and Windows Graphics Capture. Frames are taken
  in order by sequence number, so timing jitter no longer decides which frame
  is shown. Faster games are decimated by the measured ratio (even 2:1 for
  120 → 60).
- **Constant low latency.** When the clock difference accumulates a spare
  frame, exactly one frame is dropped; the picture never drifts behind the
  audio.
- **Match Display** sets the OBS frame rate to the display's measured rate, so
  no correction is needed at all.
- **Limit capture framerate** rewritten; it no longer collapses to ~30 FPS.
- **Windows Graphics Capture** no longer loses a frame every few seconds on
  Windows 11 24H2+ (its default delivery limit is lifted).
- Preview latency two frames; OBS graphics thread registered with MMCSS.

Measurements: [Technical notes → Validation](CAPTURE_STUTTER_FIX.md#validation).

</details>

<details>
<summary>Previous releases</summary>

- **v1.1.0** fixed Game Capture and DXGI Display Capture and added Match
  Display. Window Capture and Display Capture in *Windows 10 (1903 and up)*
  mode were unchanged.
- **v1.0.1** removed the long episodes but absorbed the clock difference by
  accumulating delay: in a 20-minute recording the picture fell behind the
  audio from about 25 ms to 80 ms. Update to the latest release.

</details>

<details>
<summary>Environment variables (troubleshooting only)</summary>

| Variable | Effect |
|---|---|
| `OBS_READY_QUEUE=0` | Disable the Game Capture queue (stock path). |
| `OBS_DXGI_READY_QUEUE=0` | Disable the DXGI Display Capture queue. |
| `OBS_WGC_READY_QUEUE=0` | Disable the Windows Graphics Capture queue. |
| `OBS_READY_QUEUE_POLICY=` | `lowlatency` (default), `bounded` (v1.0.1 behavior), `latest` (newest frame). |

</details>

<details>
<summary>Build it yourself</summary>

Releases are built by GitHub Actions
(`.github/workflows/capture-stutter-windows.yaml`); the installer is
`.github/installer/capture-stutter-fix.nsi`. A local build needs Visual Studio
2026 with *Desktop development with C++*, Windows 11 SDK 10.0.26100, CMake 4.2+
and Git:

```
git clone --recurse-submodules https://github.com/veelasama/obs-studio-capture-stutter-fix.git
cd obs-studio-capture-stutter-fix
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
cmake --install build_x64 --prefix build_x64/install --config RelWithDebInfo
```

</details>

## Support

This is a focused experimental fork, not a separately supported OBS
distribution. The [feedback discussion](https://github.com/veelasama/obs-studio-capture-stutter-fix/discussions/2)
only asks whether the build helped with the symptom above; for anything else
use the official OBS Project support channels.

Credits: [FORK_AUTHORS.md](FORK_AUTHORS.md). OBS Studio remains the work of the
OBS Project contributors. All changes are distributed under GPL-2.0-or-later.
