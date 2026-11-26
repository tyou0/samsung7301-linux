# Samsung 04e8:7301 fingerprint sensor — Linux driver

Userspace driver for the **Samsung `04e8:7301`** fingerprint sensor found in
the Samsung Notebook 9 (`NT900X5V` / `900X5T`).

The repository contains three things:

* **[`contrib/samsung7301.c`](contrib/samsung7301.c)** — a libfprint driver so
  that the stock `fprintd` can use the sensor.
* **[`contrib/sourceafis-matcher/`](contrib/sourceafis-matcher/)** — the
  native matcher helper the driver calls.  It runs the real SourceAFIS 3.14
  matcher over the native frames, because the frames of this sensor do not
  carry enough minutiae for libfprint's own NBIS/bozorth3 path.
* **`samsung7301`** — a Rust implementation of the sensor protocol
  ([`src/`](src)).  It is the research tool the configuration was recovered
  with.  It still applies the legacy experimental register set of the
  Windows driver and is **not** a calibrated working pipeline.

The sensor had **no driver at all** on Linux: `fprintd` did not even know the
device existed (`NoSuchDevice: No devices available`).  All parts were written
from scratch by reverse engineering Samsung's own Windows driver.

## Status (2026-10-25)

| | |
|---|---|
| `fprintd` device discovery | works |
| Driver | plain `FpDevice`, native 160x160 frames; verification checks after every 4 frames up to 16; enrollment uses 16 frames; 80 ms spacing, 150 ms settle |
| Matcher | SourceAFIS 3.14 in a separate native helper, threshold 40, never lowered |
| Stored print | protected `FPI_PRINT_RAW` print, version `samsung7301-sourceafis-v1` |
| Enrolment | **completed** — all 5 stages passed, the print is stored in `fprintd`'s own storage |
| Verification (genuine) | **works** — `fprintd` reports `verify-match` |
| Verification (different finger) | **not run** — a different finger has not been tried |
| `sudo` unlock with a fingerprint | **not validated** — a successful direct sudo authentication has not been observed |
| Sandbox / PAM | `fprintd`'s sandbox was not changed; the PAM stack of `sudo` and `polkit-1` has `auth sufficient pam_fprintd.so` |

Enrolment and verification through `fprintd` work. On 2026-10-25, the
optimized driver verified a fingerprint in **1.21 seconds** during system
authorization. Direct sudo authentication and login-screen timing remain
unvalidated. Earlier missed touches remain unexplained; one successful
verification does not establish detection reliability.

### The installed stack

The driver and the helper are installed on this machine:

| file | sha256 |
|---|---|
| `/usr/local/lib/libfprint-2.so.2.0.0` | `8737b210534fa5610c396247e2ccd005efe065750d2f4a46a04e333c6b92be55` |
| `/usr/local/libexec/libfprint-samsung7301-afis` | `2671d4360cff3c0f0c962921b4b6a4411fd23767fc163dfc68442a10f46ccae0` |

The helper is `root:root` with mode `755`.  The driver starts it with a
fixed absolute path, without a shell and without any argument; the path is
not taken from the environment, from a print or from any other source.

The driver no longer uses libfprint's image device interface.  It is a plain
`FpDevice` that captures the native 160x160 frames itself, 80 ms apart after
the finger has rested on the sensor for 150 ms, and hands the frames to the
helper over a pipe. Verification checks after every 4 frames and, if
necessary, continues the same burst to 16 frames. Time spent on an
intermediate check counts toward the minimum 80 ms before the next frame.
Verification restores the detection profile and finishes without waiting
for finger removal. Enrollment still uses
16 frames and requires finger removal between stages. The helper prints one result
line, the driver validates that line and the exit status of the helper, and
the run is bounded by 15 seconds.

The image preparation inside the helper is the one that was validated with
the sensor: a per frame min/max linear contrast stretch with integer
rounding, then the inversion, then SourceAFIS at 500 dpi.  The match
threshold is SourceAFIS's own 40; it is not lowered anywhere.

### The stored print

The enrolled print is an `FPI_PRINT_RAW` print whose data is the variant
`(saay)`: the version string `samsung7301-sourceafis-v1` and the **selected
original native frames** themselves — at most 4 per enrolment stage, 5
stages, so at most 20 frames.  fprintd keeps its usual storage for it.

A print of any other layout — in particular an **old NBIS print** from the
previous driver — cannot be used and is refused with an explicit error that
asks for a new enrolment.  Existing fingers therefore have to be enrolled
again after the switch to this stack.

### What has been checked

* The libfprint test suite: **5 runnable suites pass, 0 fail, 33 fixture
  tests skipped**.
* A hardware-free C harness that includes the real driver source and drives
  its static helpers: **108 checks, 0 failures**.
* The installed native helper on the retained genuine pair: a cross score of
  **40.39932150539135 in both directions**, with the helper running under
  MDWE (no writable and executable memory mapping).
* **4 hardware open / enrol-cancel / close cycles** passed, including one
  whose cancellation timer fired after 0.01 s; that cycle completed after
  0.04 s.  No warnings.
* **Enrolment through `fprintd` with the installed stack** completed all five
  stages on 2026-10-22; the print is stored in `fprintd`'s own storage.
* **Genuine verification through `fprintd` with the installed stack** reported
  `verify-match`.

None of these is a successful authentication: they cover the build, the
wire protocol of the helper, the lifetime handling of the driver and the
matcher itself.

### Diagnostics — experimental, not a proof

Two measurements were made with the diagnostic harness in `/tmp` (the
NativeAOT prototype, not the installed helper):

| probe | bank 0 | bank 1 | pairs at or above 40 |
|---|---|---|---|
| fresh right index | 58.05 | 48.34 | — |
| right middle (a different finger) | 14.34 | 14.68 | 0 of 810, both banks |

The numbers are promising, and the different finger stays well below the
threshold of 40.  But they were measured by a **diagnostic program in
`/tmp`, against frames that were retained in RAM** — they are **not** the
installed driver, and they are **not** a proof that `fprintd` authenticates
anyone.  The installed stack still has to prove itself end to end.

### Still open

* A **different-finger rejection through `fprintd`** with the installed stack
  has **not been run**.
* Direct `sudo` authentication and login-screen timing are **unvalidated**.
* Before early matching, the **capture burst was shortened**: 16 frames instead of 32.  The burst
  dominated the time of an attempt, and the matcher selects at most four of
  the frames.  Measured with the real stored frames, the best cross score is
  already the same for 12 probes as for 15, so the longer burst only added
  waiting time.  Measured on the sensor, one attempt then took **3.1 s**
  (2.8 s of burst and 0.3 s of matching) instead of **6.5 s**; the
  verification still reports `verify-match`.

### Verification latency fix (2026-10-23)

The installed C driver now checks verification after 4 frames, falls back to
16 frames when needed, and finishes without waiting for finger removal.
The match threshold and enrollment behavior are unchanged. The build,
regression checks, and independent review passed. A successful four-frame
verification on 2026-10-25 took **1.21 seconds** from finger detection to
`verify-match`. Earlier tests reported zero contact zones despite the user
holding a finger on the reader; that intermittent failure remains unexplained.
See [latency measurements and installation status](docs/LIBFPRINT-STATUS.md#verification-latency-fix-2026-10-23).

## Quick start

The matcher helper has to be installed **before** the driver is used: the
driver refuses to run without it.

```sh
# 1. build and install the native matcher helper at its fixed path
#    (the .NET 10 SDK and the SourceAFIS 3.14.0 package are needed)
cd contrib/sourceafis-matcher
dotnet publish -c Release -r linux-x64 -o /tmp/sourceafis-production-out
sudo install -d -m755 /usr/local/libexec
sudo install -m755 /tmp/sourceafis-production-out/libfprint-samsung7301-afis \
              /usr/local/libexec/libfprint-samsung7301-afis
sudo chown root:root /usr/local/libexec/libfprint-samsung7301-afis
cd ../..
#    the full instructions are in contrib/sourceafis-matcher/README.md

# 2. permissions for the sensor
sudo install -m644 contrib/99-samsung-7301.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger

# 3. build and install the libfprint driver
git clone https://gitlab.freedesktop.org/libfprint/libfprint.git
cd libfprint
cp /path/to/samsung7301-linux/contrib/samsung7301.c libfprint/drivers/
# add it to libfprint/meson.build and meson.build, see docs/LIBFPRINT.md
meson setup build -Ddoc=false -Dintrospection=false
ninja -C build && sudo ninja -C build install
sudo ldconfig
sudo systemctl restart fprintd
cd ..

# 4. let fprintd use the reader without a polkit prompt (optional, see the
#    comment in the rule)
sudo install -m644 contrib/49-samsung7301.rules /etc/polkit-1/rules.d/

# 5. enrol a finger — a FRESH enrolment, the old prints cannot be used
fprintd-enroll -f right-index-finger "$USER"
```

Step 5 is a **fresh** enrolment and it takes the five stages of the driver:
hold the finger flat and still on the reader for about **8 seconds** per
stage, **lift it for about 2 seconds** between the stages, and repeat that
five times.  Lift it completely between the stages — a finger that stays on
the reader is not a new stage.

**No success is claimed for the `sudo` unlock.**  Enrolment and a genuine
verification through `fprintd` were run on 2026-10-22 with the installed stack
and both worked; a different-finger rejection and a `sudo` unlock have not been
run.  Treat the password as the working credential until they are.

`pam_fprintd.so` is in the `auth` stack of `/etc/pam.d/sudo` and
`/etc/pam.d/polkit-1` (see [`contrib/pam_fprintd`](contrib/pam_fprintd));
`/etc/pam.d/sudo.bak-samsung7301` keeps the original file.  A `sudo` unlock
with a fingerprint is **not validated** with a successful authentication, so
keep the password as the working credential.  Nothing in `fprintd`'s sandbox
was changed.

## The sensor

| | |
|---|---|
| Laptop | Samsung Notebook 9 `NT900X5V-A39A` |
| USB ID | `04e8:7301` "Fingerprint Device" |
| Chip | FPC (Fingerprint Cards AB) FPC1021, revision `0x1f` |
| Image | 160 x 160 pixels, 8 bit |
| Endpoints | bulk OUT `0x01`, bulk IN `0x82`, interrupt IN `0x83` |

## Protocol

The sensor speaks the FPC "wbdi" transport.  Every command is bracketed by
vendor control requests on EP0:

```
1. CTRL-OUT  bRequest=0xc3  wValue=0x0000 wIndex=0x0000  16 byte heartbeat
2. CTRL-OUT  bRequest=0xca  wValue=0x0003 wIndex=<reply length>
3. BULK-OUT  the command bytes                            (EP 0x01)
4. BULK-IN   <reply length> bytes                        (EP 0x82)
5. CTRL-IN   bRequest=0xda  wValue=0x0007  2 bytes       (doorbell, optional)
```

The session is opened with four `0xdb` writes (`wValue` 0x0006 and 0x000b,
`wIndex` 0 and 1).  The reply length of a command is always the length of
the command itself.

Two details are easy to get wrong and cost a lot of time:

* **The image read has to arm the bulk IN before the bulk OUT is written.**
  The sensor starts streaming the frame while the host is still writing the
  padded command, so a sequential read only ever sees the tail of the frame.

* **The generic analogue tables of the Windows driver must not be written.**
  With them the sensor reported a finger on an untouched reader and the
  frames stayed flat.  The driver therefore keeps the factory analogue
  registers and only writes the gain and the detection threshold of the
  capacitive detection (`a0 06 02` and `d8 60`) plus the capture gain
  (`a0 09 02`).  The legacy tables only survive in the Rust research tool.

The full account of the reverse engineering — including the Windows driver's
per-chip configuration table, how it was recovered, and the USB capture it was
cross checked against — is in
[`docs/REVERSE-ENGINEERING.md`](docs/REVERSE-ENGINEERING.md).  The command
set is documented in [`PROTOCOL.md`](PROTOCOL.md).

## Using the Rust tool

```sh
cargo build --release

sudo ./target/release/samsung7301 info       # descriptors, sends nothing
sudo ./target/release/samsung7301 session    # chip identification
sudo ./target/release/samsung7301 init       # bring-up sequence
sudo ./target/release/samsung7301 capture    # one image -> captures/raw.bin
```

This is the research instrument the configuration was recovered with.  It
still writes the legacy experimental register set, so its frames and its
detection behaviour are **not** the calibrated pipeline and it should not be
used as a substitute for the libfprint driver.

## Historical experiments — not the current behaviour

The stack above replaced an earlier NBIS/bozorth3 approach, which is kept
here only so that the old numbers are not mistaken for the current ones.
That earlier driver was an `FpImageDevice`: it enlarged the native 160x160
frame 2x to 320x320, waited 150 ms after the finger was detected, took a
burst of three frames 80 ms apart and published the frame with the most
features — but only if that frame had at least 10 of them.  Its genuine
verification scored **0, 3, 7, 0 and 6 of 40**, so it never matched.  The
library build of that era had the sha256
`596f5d5281518451637c7cc9e1489414290a4e0d7933b6226816dc815f248ecc`
(the build before that, without the explicit USB interface claim, was
`fd5ccc9a9ed9a5a19857010595a9f026262c5feca1e142054587a4bf89846699`).

The feature count of that path was necessary but not sufficient: a frame could
clear the 10 features and still not match itself, and no scaling or contrast
variant fixed the tested captures.  A swipe-assembly diagnostic over a larger
finger area did not reach the threshold either.  The detailed measurements
are in [`docs/LIBFPRINT-STATUS.md`](docs/LIBFPRINT-STATUS.md).

## Known limitations and open work

* A different-finger rejection through `fprintd` with the installed stack has
  not been run.
* No successful biometric authentication (`sudo`, login) has been observed —
  biometric authentication is still unresolved.
* The prints of the old NBIS driver are unusable and have to be enrolled
  again.
* The hardware identification is only accepted for `02 1f`; another one is
  reported instead of being served with a guessed configuration.
* Whether the earlier transient IRQ ff reset has any relation to the missing
  USB interface claim is **not proven**; the claim fix removed the kernel
  warning about it, and nothing more is known.

## License

MIT for the Rust code, LGPL-2.1-or-later for the libfprint driver and for
the matcher helper (they have to match libfprint's license).  See
[`LICENSE`](LICENSE).

This project is unaffiliated with Samsung.  Reverse engineering for
interoperability was carried out on hardware the contributor owns.
