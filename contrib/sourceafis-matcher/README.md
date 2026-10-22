# libfprint-samsung7301-afis — the SourceAFIS matcher of the samsung7301 driver

This directory holds the matcher helper of the `samsung7301` libfprint driver
([`../samsung7301.c`](../samsung7301.c)).

The frames of the sensor do not carry enough minutiae for libfprint's
NBIS/bozorth3 path, so the driver does not use the image device interface.
It captures the native 160x160 frames itself and hands them to this helper,
which runs the real SourceAFIS matcher over them.  The helper is a separate
program because libfprint is C and SourceAFIS is a .NET library; the driver
never links against it.

The helper is deliberately small and boring:

* it reads one binary blob from stdin and writes one line to stdout,
* it never touches a file, the network or an image codec,
* it never serializes a template — the templates live in RAM only,
* it writes nothing to stderr when it succeeds, and
* every failure exits with a non zero status and prints no result line.

## Requirements

* the .NET 10 SDK (for the build),
* the `SourceAFIS` 3.14.0 package (restored from nuget.org), and
* a Linux x86-64 host for the published binary.

## Building and publishing

The project publishes a NativeAOT executable: one native binary, no shared
runtime, no JIT and no ICU.

```sh
cd contrib/sourceafis-matcher
dotnet publish -c Release -r linux-x64 -o /tmp/sourceafis-production-out
```

The result is a single file:

```
/tmp/sourceafis-production-out/libfprint-samsung7301-afis
```

`PublishAot`, `InvariantGlobalization` and `RuntimeIdentifier` are set in
[`SourceAfisMatcher.csproj`](SourceAfisMatcher.csproj), so no extra flags are
needed.  `InvariantGlobalization` keeps the output independent of the locale
of the machine.

### The warnings are not suppressed

The publish prints the advisories of the transitive dependencies
(`SixLabors.ImageSharp`, `NU1902`/`NU1903`) and the AOT trim warnings of
`Dahomey.Cbor` (`IL2104`/`IL3053`).  They are left visible on purpose: the
dependency graph of `SourceAFIS` is what it is, and hiding the warnings
would hide it.  Neither library is used on the matching path — the matcher
works on the raw pixel buffer, it does not encode or decode an image and it
does not serialize a template — but they are part of the package and their
warnings belong into the review.

## Installation

The driver runs the helper from a fixed path, so the file has to be there and
it has to not be writable by anybody but root.  The source of the install is
the published binary, not the project directory:

```sh
sudo install -d -m755 /usr/local/libexec
sudo install -m755 /tmp/sourceafis-production-out/libfprint-samsung7301-afis \
              /usr/local/libexec/libfprint-samsung7301-afis
sudo chown root:root /usr/local/libexec/libfprint-samsung7301-afis
```

The driver starts the program with an absolute path, without a shell and
without any argument.  The path is not taken from the environment, from a
print or from any other source, so the file above is the only place the
helper is ever read from.

## The protocol

### Input

stdin carries exactly one blob, little endian:

| offset | size | content |
|---|---|---|
| 0 | 8 | the magic, ASCII `S731AF01` |
| 8 | 4 | `u32` the number of probe frames, 1..32 |
| 12 | 4 | `u32` the number of reference frames, 0..32 |
| 16 | 25600 × probes | the probe frames |
| … | 25600 × references | the reference frames |

A frame is one native sensor frame: 160x160 pixels, 8 bit grey, without a
header and without any resizing.  The stream has to end after the announced
frames; a trailing byte is an error, and so is a short stream.

The helper reads the header first, validates the counts and then reads
exactly the announced number of bytes, so it never allocates more than the
protocol allows.

### Output

On success exactly one line is printed, ended by a newline:

```
OK <quality_max> <best_cross> <selected_count> <i0> <i1> <i2> <i3>
```

* `quality_max` — the highest capture strength of the probe frames,
* `best_cross` — the highest identity score between an eligible probe and
  the references, `0` when there are no references or no eligible probe,
* `selected_count` — how many frames were selected, 0..4,
* `i0`..`i3` — the indices of the selected probe frames in ascending order,
  `-1` for the unused slots.

The scores are printed with the full precision of a double and in the
invariant format.  They are on the scale of SourceAFIS and are not bounded
from above.

On any error nothing is printed on stdout, a message is written to stderr
and the exit status is non zero.

## The scoring

The image preparation is the one that was validated with the sensor: a per
frame min/max linear contrast stretch with integer rounding, then the
inversion, then SourceAFIS at 500 dpi:

```
span    = max - min
stretched = ((pixel - min) * 255 + span / 2) / span
pixel   = 255 - stretched
```

A frame whose raw standard deviation is below 2 carries no ridge information
and is skipped — it has no template at all.

* **self** of a probe is its exact self match, that is
  `Match(template(probe), template(probe))`.  It measures how well the finger
  was captured.  A probe is *eligible* when its self score is at least 40.
  The self score is a strength measure only and is never used as an identity
  decision.
* **cross** of a probe is the exact score of the probe against the reference
  frames.  This is the identity measure.

The selected frames are the eligible probes with the highest self score, at
most four of them, each of them a distinct frame.  When there are references,
a probe is only selected when its cross score is at least 40 as well — that
is what keeps a different finger out of a template.

A SourceAFIS extraction or match error aborts the whole run with a non zero
exit status.  It is never turned into a score.

## What the driver does with the result

The driver validates the exit status and the line before it acts on it: one
line, eight fields, finite and non negative scores, a frame count of 0..4,
indices that are inside the burst, distinct and marked as unused where they
are unused, and a selected frame that is eligible and that matches the
references when there are references.  A result that does not satisfy this is
an error, never a match.

The lifecycle of the helper is bounded on the driver side:

* the driver writes the frames to the helper over a pipe and reads its
  output, and it waits for the process itself,
* the run is bounded by 15 seconds; when the helper takes longer it is
  killed,
* a cancellation kills the helper as well,
* and the operation is only completed when the child is really gone — a
  failed communication is not enough, the child is reaped first, so that
  nothing is reported while a process is still running.

The enrolled print is a `FPI_PRINT_RAW` print whose data is the variant
`(saay)`: the version string `samsung7301-sourceafis-v1` and the native
frames themselves.  The frames are stored in the print, so fprintd keeps its
usual storage and nothing about the way fprintd stores or writes a print has
to change.

## Licence

LGPL-2.1-or-later, the same licence as the driver.
