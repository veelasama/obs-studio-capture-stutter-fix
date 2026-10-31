# Contributing to the Capture Stutter Fix fork

This repository is a focused downstream build of OBS Studio. Its purpose is to
maintain and test the completed-frame queue used by Windows Game Capture and
DXGI Desktop Duplication. It is not intended to become a general alternative
support channel or a second full OBS distribution.

Pull requests are welcome when they directly improve the capture-stutter patch,
its fallback behavior, Windows build reproducibility, installer, documentation,
or compatibility with a newer stable OBS release. General OBS features and
bugs that are unrelated to this patch belong in the upstream OBS Project.

A useful pull request should explain the exact capture path it changes, why the
change is needed, and how it was tested. Changes to GPU synchronization must
preserve the stock fallback path. Windows source changes should compile for the
x64 OBS build and for both the 64-bit and 32-bit Game Capture hooks when those
hooks are affected. Keep commits reviewable and do not include generated build
directories or binaries in source commits.

AI-assisted work is allowed. The contributor is responsible for understanding,
reviewing, testing, and licensing everything they submit, regardless of which
tools helped produce it. Low-quality unverified output may be closed for the
same reason as any other incorrect change; the tool used to write it is not the
criterion.

All contributions to this fork are distributed under the same
GPL-2.0-or-later terms as OBS Studio. By submitting a pull request, you confirm
that you have the right to provide the contribution under those terms.

The public Discussion asks only whether the released build helped with the
recognizable intermittent-stutter symptom. It is not a bug intake queue and
detailed user diagnostics are not expected.
