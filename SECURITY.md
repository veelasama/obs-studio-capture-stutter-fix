# Security policy for this fork

This repository is an experimental downstream build of OBS Studio and does not
operate a separate bug bounty, response-time commitment, or public security
program.

If a vulnerability also affects unmodified OBS Studio, report it through the
[official OBS Project security channels](https://github.com/obsproject/obs-studio/security/policy).
That gives the upstream maintainers the best chance to protect every OBS user.

If the vulnerability is introduced specifically by this fork's ready-frame
queue, installer, or release workflow, use **Security → Report a
vulnerability** in this repository. Do not put exploit details in the public
feedback Discussion. Reports should state which released installer was tested
and why the behavior is specific to the fork.

Installing unsigned experimental software is not by itself a vulnerability.
Release downloads include SHA-256 checksums so users can verify that the file
matches the asset published by this repository.
