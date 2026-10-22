# Integrating the driver into libfprint

The driver in [`../contrib/samsung7301.c`](../contrib/samsung7301.c) is a
libfprint driver, but not an image device driver: the frames of this sensor
do not carry enough minutiae for libfprint's NBIS/bozorth3 path, so the
driver is a plain `FpDevice` that captures the native 160x160 frames itself
and hands them to a fixed matcher helper.

libfprint does not load drivers from a plug-in directory, so the driver has
to be built into the library.  The helper is a separate program and has to
be installed **before** the driver is used.

## 1. Build and install the matcher helper

The helper is a NativeAOT .NET program that runs the real SourceAFIS 3.14
matcher.  It is published as a single native binary and installed at the
fixed path the driver expects:

```
/usr/local/libexec/libfprint-samsung7301-afis
```

The complete instructions — the .NET SDK, the `dotnet publish` command, the
install of the file as `root:root` with mode `755` and the wire protocol of
the helper — are in
[`../contrib/sourceafis-matcher/README.md`](../contrib/sourceafis-matcher/README.md).

In short:

```sh
cd contrib/sourceafis-matcher
dotnet publish -c Release -r linux-x64 -o /tmp/sourceafis-production-out
sudo install -d -m755 /usr/local/libexec
sudo install -m755 /tmp/sourceafis-production-out/libfprint-samsung7301-afis \
              /usr/local/libexec/libfprint-samsung7301-afis
sudo chown root:root /usr/local/libexec/libfprint-samsung7301-afis
```

The driver starts the program with an absolute path, without a shell and
without any argument.  The path is not taken from the environment, from a
print or from any other source, so the file above is the only place the
helper is ever read from.

## 2. Copy the driver in

```sh
cp contrib/samsung7301.c /path/to/libfprint/libfprint/drivers/
```

## 3. Register it

`libfprint/meson.build` — add the driver to the source list:

```meson
    'fpcmoc' : files('drivers/fpcmoc/fpc.c'),
+   'samsung7301' : files('drivers/samsung7301.c'),
```

`meson.build` — add it to the driver table:

```meson
    'fpcmoc': {},
+   'samsung7301': {},
```

## 4. Build and install

```sh
meson setup build -Ddoc=false -Dintrospection=false
ninja -C build
sudo ninja -C build install
sudo ldconfig
sudo systemctl restart fprintd
```

The driver is now built into `libfprint-2.so`; `fprintd` picks it up
automatically.

## 5. udev rules

libfprint's own rules only cover the devices it knows about, so the sensor
needs a rule of its own:

```
SUBSYSTEM=="usb", ATTR{idVendor}=="04e8", ATTR{idProduct}=="7301", TAG+="uaccess"
```

See [`../contrib/99-samsung-7301.rules`](../contrib/99-samsung-7301.rules).

## 6. Enrol a finger

The enrolment is a **fresh** one: the prints of the earlier NBIS driver are
of an incompatible layout and are refused with an explicit error.

```sh
fprintd-enroll -f right-index-finger "$USER"
```

The driver has five enrolment stages.  Hold the finger flat and still on the
reader for about **8 seconds** per stage, **lift it for about 2 seconds**
between the stages, and repeat that five times.  The finger has to be lifted
completely between the stages — a finger that stays on the reader is not a
new stage.

No success is claimed for this procedure.  See
[`LIBFPRINT-STATUS.md`](LIBFPRINT-STATUS.md) for what has been measured and
what is still pending.

## The profile of the sensor

The driver keeps the analogue front end the sensor was shipped with and
writes only the gain and the detection threshold.  The sensor therefore has
to be factory reset once; the driver never writes these registers:

| register | factory value |
|---|---|
| `adcsetup` | `9c 55 40 00 3f 24` |
| `adcweighttbl` | `98 03 e9 bf 37 fe 05 19 32 9b 66` |
| `pxlctrl` | `a8 0f 1b` |
| `fngrdriveconf` | `8c 02` |
| `imagerd` | `64 0e` |
| `imagesetup` | `5c 03` |

What the driver writes:

| command | when |
|---|---|
| `d8 60` (`fngrdetthresh`) | once, when the device is opened |
| `a0 06 02` (`adcshiftgain`) | on every activation, to restore the detection profile |
| `a0 09 02` (`adcshiftgain`) | before an exposure, the capture gain |
| `54 00 a0 00 a0` (`imgcapsize`) | before an exposure, the 160x160 geometry |

The generic analogue tables of the Windows driver are not used: with them the
sensor reports a finger on an untouched sensor and the frames stay flat.
They are still in the Rust research tool in [`../src/`](../src), which is
the tool the configuration was recovered with.

## The capture

One press of the finger is captured as a **burst of 32 real frames**, 80 ms
apart.  The finger has to rest on the sensor for 150 ms before the first
exposure.  Every frame of the burst is a real frame of the sensor and all of
them are kept; the matcher decides which of them carry any ridge
information.

## The stored print

The enrolled print is an `FPI_PRINT_RAW` print whose data is the variant
`(saay)`:

| field | content |
|---|---|
| version | the string `samsung7301-sourceafis-v1` |
| frames | the selected **original native frames**, 160x160 8 bit grey, without a header |

At most 4 frames are kept per enrolment stage and there are 5 stages, so a
print holds at most 20 frames.  The frames are stored in the print, so
`fprintd` keeps its usual storage and nothing about the way `fprintd` stores
or writes a print has to change.

A print of any other layout — in particular an old NBIS print — is refused
with an error that asks for a new enrolment.

## Notes on the implementation

* **The state machine has to be created per activation.**  libfprint frees an
  `FpiSsm` once it completes, so the driver creates a new one in
  `s7301_dev_activate()` and a new one in `s7301_dev_open()`.

* **Command buffers need static storage.**  The USB transfers are
  asynchronous, so the command bytes must stay valid until the transfer
  completes; a compound literal inside a `switch` case does not.

* **The image read arms the bulk IN before the bulk OUT.**  See
  `s7301_img_after_prime()`.

* **The helper is a fixed, root owned program.**  It is started without a
  shell, with a fixed path and without any argument; the path is not taken
  from the environment, from a print or from any other source.

* **The result of the helper is validated before it is used.**  The driver
  checks that the output is exactly one line with eight fields, that the
  scores are finite and non negative, that the selected frame count is 0..4,
  that the indices are inside the burst, distinct and marked as unused
  where they are unused, and that a selected frame is eligible and matches
  the references when there are references.  A result that does not satisfy
  this is an error, never a match.

* **The run of the helper is bounded.**  The driver waits for the process
  itself, kills it after 15 seconds, kills it on a cancellation and only
  completes the operation when the child is really gone — a failed
  communication is not enough, the child is reaped first, so that nothing is
  reported while a process is still running.

* **The threshold is 40 and it is not lowered.**  It is SourceAFIS's own
  threshold, and it is used for the capture strength (the self match) and
  for the identity decision (the cross match) alike.  With references
  present, a probe is only selected when it also matches the references, so
  that a different finger can never be mixed into a template.

* **The exposure is polled before the frame is read.**  After the trigger the
  driver reads the interrupt register until the sensor reports that the
  image is ready, and fails the capture when the sensor signals an error or
  a reboot instead.

* **The finger is decided by the capacitive zones.**  The interrupt bits are
  not a finger proxy: the driver reads the twelve zones and reports a finger
  when at least seven of them are covered.

* **Only one hardware identification is accepted.**  The profile was
  verified with `02 1f`, and any other identification is reported instead
  of being served with a guessed configuration.  The Windows driver itself
  masks the high nibble of the revision, so its tables cover the revisions
  `0x10` to `0x1f`; `0x1f` is not unknown to it.

* **The USB interface is claimed explicitly** and released again when the
  device is closed; the interface is released as well when the open fails.

* **The driver implements `open`/`close`**, not `img_open`/`img_close`: it is
  a plain `FpDevice` and does not use the image device interface.
