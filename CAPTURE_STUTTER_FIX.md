# OBS Studio Capture Stutter Fix

This fork addresses a specific intermittent frame pacing failure in capture,
first reproduced on GeForce RTX 5060, 5060 Ti, and 5070 Ti systems.

## Recognizable symptoms

- The game itself remains smooth on the monitor.
- OBS continues to report the configured frame rate, usually 60 FPS.
- OBS reports no rendering lag and no encoder overload.
- The preview and the recorded file visibly fall to roughly 30–40 unique
  frames per second, or show duplicate-frame skips.
- A bad interval can begin only after several minutes, last from a fraction of
  a second to tens of seconds, and then recover by itself.
- Preview stutter can occur while recording is stopped.
- Changing encoder, bitrate, quality, game settings, driver version, Windows
  version, or even rebuilding the PC may not remove it.

This symptom set matters. Ordinary GPU overload, encoder overload, an unstable
game frame rate, network loss, and playback-only judder have different causes.

## What changed

The patch separates capture completion from the frame OBS is currently
rendering. Producers publish completed GPU frames into an eight-slot shared
texture queue. OBS consumes a completed frame on its video tick through shared
fences and keeps a bounded backlog instead of repeatedly sampling one mutable
shared texture at an unlucky phase.

The Windows graphics thread also joins MMCSS using the `Playback` task at high
relative priority. Preview swap-chain latency is set to two frames. DXGI
Desktop Duplication uses a dedicated D3D11 producer with eight shared textures
and producer/consumer fences; pointer-only updates do not become video frames.

All paths fail back to the stock OBS capture path if queue initialization,
resource sharing, or a GPU fence fails.

## Capture paths

The ready-frame queue is implemented for Windows Game Capture with:

- Direct3D 9
- Direct3D 10
- Direct3D 11
- Direct3D 12
- OpenGL
- Vulkan
- 64-bit and 32-bit game hooks

It is also implemented for Display Capture using DXGI Desktop Duplication.
Windows Graphics Capture (WGC) is unchanged.

The patch does not restrict the game to 60 FPS. Games may run at 120, 144, 240
FPS, or another rate while OBS records at its configured output rate. The
existing **Limit capture framerate** source option remains usable; it is not a
requirement for the fix.

## Validation so far

- Warhammer 40,000: Rogue Trader, Direct3D 11: the original long periodic
  stutter series was removed in repeated user tests.
- Satisfactory, Direct3D 12: the user reported an immediate visible
  improvement.
- DXGI Desktop Duplication: a 45-second 60 FPS lab capture produced 2,700
  frames with zero adjacent duplicate pairs and zero timestamp errors. One
  acquisition timeout was absorbed without disturbing output cadence.
- The final DXGI binary was reproduced from the source in this branch.

Long sessions have occasionally contained isolated short hitches. The tests do
not establish that every hitch has the same cause, and they do not prove a
universal fix for every GPU, game, compositor, plug-in, or capture device.

## Defaults and recovery

The fix is enabled by default in release builds from this fork. To isolate a
regression, start OBS with either variable set to `0`:

```powershell
$env:OBS_READY_QUEUE = '0'       # Disable the Game Capture consumer queue
$env:OBS_DXGI_READY_QUEUE = '0'  # Disable the DXGI Display Capture queue
& 'C:\Program Files\obs-studio-capture-stutter-fix\bin\64bit\obs64.exe'
```

`OBS_READY_QUEUE_POLICY=latest` selects the newest completed Game Capture frame
instead of the default bounded policy. The tested release uses the bounded
policy.

The custom installer uses
`C:\Program Files\obs-studio-capture-stutter-fix` and does not replace the
official OBS installation. It is unsigned, so Windows SmartScreen may show an
unknown-publisher warning. Verify the SHA-256 values attached to the release.

## Build it yourself

The release tag and GitHub Actions workflow are the reproducible path:

1. Fork or clone this repository with submodules.
2. Open **Actions → Windows Capture Stutter Build → Run workflow**.
3. Download the installer from the `capture-stutter-windows-x64` artifact.

For a local Windows build with the OBS build prerequisites installed:

```powershell
git submodule update --init --recursive
pwsh .github/scripts/Build-Windows.ps1 -Target x64 -Configuration RelWithDebInfo
```

The workflow then packages the CPack output into an NSIS installer. Its source is in
`.github/installer/capture-stutter-fix.nsi`.

## Reporting results

Use GitHub Discussions for general test results and the capture-stutter issue
form for reproducible regressions. Include:

- GPU and driver version
- Windows build
- game and graphics API
- capture source and OBS output FPS
- whether the preview, recording, or both stuttered
- the approximate timestamps
- an OBS log from the same session
- whether disabling either queue changes the result

This repository is an independent experimental fork. For unrelated OBS bugs,
use the official OBS Project support channels.
