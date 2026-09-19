# OBS Studio 32.2.2 Capture Stutter Fix v1.0.0

This independent Windows build targets periodic duplicate frames and cadence
drops in OBS preview and recordings while the game remains smooth, OBS stays at
its target FPS, and OBS reports no rendering or encoder overload.

It adds a GPU-completed-frame queue for Game Capture on Direct3D 9/10/11/12,
OpenGL, and Vulkan, plus a separate queue for DXGI Desktop Duplication. WGC is
unchanged. See [CAPTURE_STUTTER_FIX.md](https://github.com/veelasama/obs-studio-capture-stutter-fix/blob/master/CAPTURE_STUTTER_FIX.md)
for the design, validation, limitations, and opt-out variables.

The `...Installer.exe` file installs beside official OBS in
`C:\Program Files\obs-studio-capture-stutter-fix`.

The binaries are not code-signed. Windows SmartScreen may display an
unknown-publisher warning. Verify the installer using `SHA256SUMS.txt`.
