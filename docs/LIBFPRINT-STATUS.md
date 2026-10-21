# Local libfprint driver status (2026-10-21)

`contrib/samsung7301.c` is the experimental libfprint image driver. The Rust
research CLI and its original README describe earlier protocol research.

The local build lives at `/tmp/opencode/libfprint-upstream`; its driver copy
has been synchronized with `contrib/samsung7301.c`. The corrected library is
installed at `/usr/local/lib/libfprint-2.so.2.0.0`, which `/usr/lib/fprintd` loads.
The build directory is temporary; preserve the repository source.

## Fixed crash

At 15:20:09 KST, fprintd PID 562888 crashed on a second activation. The journal
reported `BUG: (!ssm->completed): fpi_ssm_start()` immediately before SIGSEGV.
libfprint frees state machines after their completion callback. Allocate a new
open/capture machine for every operation and clear the stored pointer on
completion.

The image path also now waits for both concurrent USB transfers before
advancing/failing the machine, owns the OUT buffer separately, propagates write
errors, and gives the captured image to libfprint without prematurely dropping
its reference. The receive buffer is released on device close.

## Validation

- Driver and libfprint build succeeded.
- libfprint fpi-ssm, fpi-device, fpi-assembling and metainfo checks passed.
- The generated hardware database check passed after regenerating the local
  source database to include Samsung 04e8:7301. 33 emulation checks were skipped.
- Six real-device captures across three open/close cycles succeeded without the
  previous crash. `tools/check-libfprint-lifecycle.py` reproduces this check;
  stop fprintd first and run with USB access. It saves no fingerprint images.
- After restarting fprintd, `fprintd-list tyou` found the Samsung sensor and
  reported no enrolled fingers.

Fingerprint enrollment, matching accuracy, and authentication remain unverified.
Next: `fprintd-enroll -f right-index-finger`, then `fprintd-verify`. Enrollment
requires the user's participation. Do not treat a capture/lifecycle check as
proof of matching accuracy.
