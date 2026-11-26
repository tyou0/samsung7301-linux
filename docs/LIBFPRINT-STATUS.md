# libfprint driver status

*Current as of 2026-10-25.*

This document describes the **current** stack, the measurements that were
made with it, and what is still open.  The earlier NBIS/bozorth3 approach
is kept in a separate section at the end and is **not** the current
behaviour.

## The installed stack

Two files are installed on this machine:

| file | sha256 |
|---|---|
| `/usr/local/lib/libfprint-2.so.2.0.0` | `8737b210534fa5610c396247e2ccd005efe065750d2f4a46a04e333c6b92be55` |
| `/usr/local/libexec/libfprint-samsung7301-afis` | `2671d4360cff3c0f0c962921b4b6a4411fd23767fc163dfc68442a10f46ccae0` |

The helper is `root:root` with mode `755`.

### The driver

The driver is a **plain `FpDevice`**, not an `FpImageDevice`.  The frames of
this sensor do not carry enough minutiae for libfprint's NBIS/bozorth3 path,
so the driver captures the native 160x160 frames itself and hands them to
the helper.

| | |
|---|---|
| frame | native 160x160, 8 bit grey, no header, no resizing |
| burst | verification checks after every 4 frames up to 16; enrollment uses 16; 80 ms apart |
| settle | 150 ms of contact before the first exposure |
| finger | decided by the capacitive zones, 7 of 12 covered |
| USB interface | claimed explicitly, released on close and on a failed open |
| threshold | 40, never lowered |

Every frame of the burst is a real frame of the sensor and all of them are
kept; the matcher decides which of them carry any ridge information.

### The helper

The helper is a separate NativeAOT program that runs the real
**SourceAFIS 3.14** matcher.  The driver never links against it: it writes
the frames into the helper over a pipe and reads one result line back.

The image preparation is the one that was validated with the sensor:

```
span      = max - min
stretched = ((pixel - min) * 255 + span / 2) / span     integer rounding
pixel     = 255 - stretched                              inversion
```

followed by SourceAFIS at **500 dpi**.  A frame whose raw standard deviation
is below 2 carries no ridge information and is skipped.

The driver starts the helper with a fixed absolute path, without a shell and
without any argument.  The path is not taken from the environment, from a
print or from any other source, so the installed file is the only place the
helper is ever read from.  The run is bounded by 15 seconds, a cancellation
kills the helper, and the operation is only completed when the child is
really gone.

The full description of the helper, its protocol and its build and install
instructions are in
[`../contrib/sourceafis-matcher/README.md`](../contrib/sourceafis-matcher/README.md).

### The stored print

The enrolled print is an `FPI_PRINT_RAW` print whose data is the variant
`(saay)`: the version string `samsung7301-sourceafis-v1` and the **selected
original native frames**.

| | |
|---|---|
| frames per stage | at most 4 |
| stages | 5 |
| frames per print | at most 20 |

The frames are stored in the print itself, so `fprintd` keeps its usual
storage and nothing about the way `fprintd` stores or writes a print has to
change.

**The prints of the old NBIS driver are incompatible.**  They are of another
layout and are refused with an explicit error that asks for a new
enrolment, so every finger has to be enrolled again.

## What has been checked

| check | result |
|---|---|
| libfprint test suite | **5 runnable suites pass, 0 fail, 33 fixture tests skipped** (`udev-hwdb`, `fpi-assembling`, `fpi-ssm`, `metainfo-validate`, `fpi-device`) |
| C harness on the real driver source | **108 checks, 0 failures** |
| installed helper on the retained genuine pair | cross score **40.39932150539135** in **both** directions, under MDWE |
| hardware lifecycle | **4 open / enrol-cancel / close cycles passed**, no warnings |
| enrolment through `fprintd` | **all 5 stages passed** on 2026-10-22, the print is stored in `fprintd`'s own storage |
| genuine verification through `fprintd` | **`verify-match`** with the installed stack, also with the 16 frame burst (`best cross 58.2265`, 4 frames selected) |

The C harness includes the real driver source, so the static helpers it
drives are the shipped ones and not a copy of them.  It touches no sensor:
no device is created, no USB transfer is made and the matcher helper is
never started.

The hardware cycles are three that were cancelled 0.4 s in and one whose
cancellation timer fired after **0.01 s**; that cycle completed after
0.04 s.  All four completed the open and the close cleanly and produced no
warnings.  The
cancelled enrolments were driven through libfprint directly, so `fprintd`
never saw the print and nothing was enrolled or stored by the system.

The MDWE run means the helper ran with the memory denial-of-write-execution
policy active: it had no writable and executable memory mapping at all.

**None of these is a successful authentication.**  They cover the build, the
wire protocol of the helper, the lifetime handling of the driver and the
matcher itself.

## Diagnostics — experimental

The following measurements were made with the **diagnostic harness in
`/tmp`** (the NativeAOT prototype), **not** with the installed driver and
helper.  They ran against frames that were retained in RAM; no raw image was
written to disk.

The two retained right-index banks were compared with a fresh capture of the
right index finger and with a capture of the **right middle finger**, a
different finger:

| probe | bank 0 | bank 1 | pairs at or above 40 |
|---|---|---|---|
| fresh right index | **58.05** | **48.34** | — |
| right middle (different finger) | **14.34** | **14.68** | **0 of 810**, both banks |

The genuine probe stays above the threshold of 40 in both banks and the
different finger stays well below it, with no pair reaching the threshold in
either bank.

These numbers are **not** a proof that the installed stack authenticates
anyone: they were produced by a separate program in `/tmp`, against retained
frames, and not through `fprintd` with the installed driver and helper.

## Verification latency fix (2026-10-23)

Before the latency fix, a successful verification took **4.83 s** from
finger detection to `verify-match`: about 3.06 s capturing 16 frames,
1.28 s waiting for the finger to be lifted, then 0.50 s matching. The
old finger-removal wait could last indefinitely while the finger stayed down.
A later successful run took **8.00 s**, including **4.53 s** waiting for
finger removal.

The installed C driver now checks a verification after **4 frames**. It accepts
an early match only when both capture quality and the identity score reach
the existing threshold of **40**. Otherwise it retains those frames and
continues to the original **16-frame** limit before deciding mismatch or
requesting another press. Verification restores the detection profile and
finishes without waiting for finger removal.

Enrollment still captures 16 frames and requires finger removal between
stages. Frame spacing, settle time, the matcher, and existing stored prints
are unchanged. The experimental Rust backend still uses its original
16-frame verification path.

The `working-state` tag preserves the user-confirmed working revision
`a58c4b4`. Its installed library was backed up at
`/tmp/samsung7301-latency-20261023/working-state/libfprint-2.so.2.0.0` before
the new library was installed and fprintd restarted successfully. The
installed-library hashes above identify the new build. The source builds
successfully and the libfprint test suite reports
5 passed, 0 failed, and 33 skipped. The hardware-free regression harness
(`python3 tools/test-samsung7301-latency.py`) also passes; it compiles the
production callbacks with stubs and checks early matching, fallback,
enrollment transitions, restoration errors, cancellation, and the next scan.
Independent code review found no actionable issues.

On **2026-10-25**, the optimized driver completed a genuine verification
in **1.21 seconds** from finger detection to `verify-match`, during system
authorization. The system journal for fprintd PID 236538 records all 12
contact zones covered, four captured frames, capture quality **69.5051**,
and cross score **45.2941**, above the unchanged threshold of 40. This is
one observed successful verification, not a measurement of lock-screen
unlock time or a reliability benchmark.

Two earlier timed tests reported zero covered zones throughout, despite
the user holding a finger on the reader. A read-only comparison of the
preserved working binary and the optimized binary found the same detection
code and register settings. The cause of those missed touches remains
unknown. The working library was temporarily restored for a comparison;
the optimized library with the hash above was then reinstalled and fprintd
restarted successfully.

## Faster matching (2026-11-26)

The journal of a weak press showed where the time went: each frame costs
about 185 ms (104 ms of USB transfer, 80 ms of spacing), and each run of the
matcher helper took 214 ms.  A press that failed the 4 frame check then had
to capture all 16 frames before it was checked again.

Two changes, neither of which touches the thresholds, the frame spacing, the
settle time or the stored prints:

* The helper builds one `FingerprintMatcher` per probe and reuses it for the
  self match and for every reference, instead of building a new matcher for
  every probe/reference pair, and extracts the templates in parallel.  On
  the retained legacy frames the result line is byte for byte the same as
  before and the helper is 1.5 to 4.2 times faster; real frames were not
  available to measure without root.
* Verification is checked after every 4 frames (4, 8, 12, 16) instead of
  only after 4 and 16.  The intermediate checks use the same threshold of
  40 for both the capture quality and the identity score, and the score of
  a subset of the frames can never exceed the score of all 16, so nothing
  is accepted that the final check would not have accepted; a genuine press
  that becomes good enough by frame 8 just finishes about 1.5 s earlier.

The hardware-free harness (`python3 tools/test-samsung7301-latency.py`)
covers the new transitions.  The rebuilt library and helper were **not
installed and not tested on the sensor** in this change.

Intermediate verification checks now count toward the minimum 80 ms delay
before triggering the next frame. After an unsuccessful check at frame 4,
8 or 12, the driver waits only for the remaining time, if any. This removes
up to 80 ms of redundant wait per checkpoint (up to 240 ms per attempt).
Enrollment and the match thresholds are unchanged.

## Still open

* A **different-finger rejection through `fprintd`** with the installed stack
  has **not been run**.
* Direct `sudo` authentication and login-screen timing are **unvalidated**.
* The earlier zero-zone detection failures remain unexplained. A later
  successful match does not establish reliable finger detection.
* The old NBIS prints have to be enrolled again; they cannot be used.
* Before early matching, one verification attempt took **3.1 s**, measured on the sensor with the
  16 frame burst: 2.8 s of burst (about 188 ms per frame, of which about
  105 ms is the USB transfer of the 25600 byte frames over a full-speed
  link) and 0.3 s of matching.  With the 32 frame burst it took 6.5 s
  (5.9 s and 0.6 s).

## The sandbox and the PAM stack

Nothing was changed in `fprintd`'s sandbox.  The PAM stack of `sudo` and of
`polkit-1` carries `auth sufficient pam_fprintd.so`, so a fingerprint is
offered there; the snippet is in [`../contrib/pam_fprintd`](../contrib/pam_fprintd).
A `sudo` unlock with a fingerprint has **not** been validated with a
successful authentication, so the password is still the working credential.

## The factory profile

The sensor has to be factory reset once; the driver keeps the factory
registers and never writes the generic analogue tables of the Windows
driver.  With those tables the sensor reported a finger on an untouched
reader and the frames stayed flat.

| purpose | registers |
|---|---|
| detection | `a0 06 02` (adcshiftgain), `d8 60` (fngrdetthresh) |
| capture | `a0 09 02` (adcshiftgain) |

The legacy tables are still in the Rust research tool in [`../src/`](../src),
which is the tool the configuration was recovered with.  That tool is a
research instrument, not a calibrated pipeline.

---

# Historical experiments — not the current behaviour

Everything below describes the **previous** NBIS/bozorth3 approach, which
was replaced by the stack above.  None of it describes how the installed
driver behaves.

## The old NBIS driver

The old driver was an `FpImageDevice`.  It enlarged the native 160x160 frame
2x to 320x320, waited 150 ms after the finger was detected, took a burst of
three frames 80 ms apart and published the frame with the most features —
but only if that frame had at least **10** of them; otherwise the press was
rejected and the driver asked for another one.  The bozorth3 threshold was
libfprint's default of 40.

The library builds of that era had these sha256 sums:

| build | sha256 |
|---|---|
| with the explicit USB interface claim | `596f5d5281518451637c7cc9e1489414290a4e0d7933b6226816dc815f248ecc` |
| before the explicit USB interface claim | `fd5ccc9a9ed9a5a19857010595a9f026262c5feca1e142054587a4bf89846699` |

Neither is installed any more; the installed library is the one with the
sha256 in the first section.

## The old verification result

The enrolment of that driver completed, with five stored samples of 12, 10,
12, 13 and 11 features.  The genuine verification against that template
**failed**: the five scored attempts gave **0, 3, 7, 0 and 6 of 40**, and
five further presses were retried because the best frame of the burst
carried too few features.  A `verify-match` had been seen much earlier with
a threshold of 15 and a different driver configuration; that was a result of
a configuration that has not been installed since and is no evidence that
anything matches.

## The old feature-count diagnostic

Three fresh, distinct touches were measured with
[`../tools/analyze-minutiae.c`](../tools/analyze-minutiae.c), one row per
touch:

| variant | feature counts | self-match score |
|---|---|---|
| native 160x160 | 4, 8, 7 | — |
| 240x240 | 5, 10, 9 | 0, 24, 0 |
| 2x 320x320, the exact pixman path of the old driver | 5, 9, 9 | 0, 0, 0 |
| 160x160, 200x200, 280x280 and 320x320 with other polarity and local-normalisation variants | — | no self score >= 40, no match across touches |

The 10 features the old driver required were therefore **necessary but not
sufficient**: a frame could clear the count and still not match itself.
Ordinary scaling and contrast variants did not fix the tested captures; that
says nothing about sizes or algorithms that were not tested.

## The old scale measurement

One and the same real frame, measured with the same analyser:

| variant | features |
|---|---|
| native 160x160 | 6 |
| 2x 320x320, normal | 12 |
| 2x 320x320, inverted | 7 |
| 3x 480x480 | 0 |
| 4x 640x640 | 0 |

That is why the old driver enlarged 2x.  The enlargement explains the
feature count only, not the match.

## The old swipe assembly

[`../tools/analyze-swipe.c`](../tools/analyze-swipe.c) assembled the frames
of a swipe into a single image, registering adjacent frames against each
other with a zero-mean normalised cross correlation.  It was never part of
the driver.  The best self score of the assembly was 34 of 40, still below
the threshold, and the second sweep was rejected as stationary, so it was
neither validated for a successful match nor for an impostor check.

---

See [`LIBFPRINT.md`](LIBFPRINT.md) for the build and install instructions
and for the implementation notes.
