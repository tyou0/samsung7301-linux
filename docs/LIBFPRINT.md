# Integrating the driver into libfprint

The driver in [`../contrib/samsung7301.c`](../contrib/samsung7301.c) is a
normal libfprint driver: an `FpImageDevice` subclass that captures frames and
lets libfprint do the minutiae detection and matching in software.

libfprint does not load drivers from a plug-in directory, so the driver has
to be built into the library.

## 1. Copy the driver in

```sh
cp contrib/samsung7301.c /path/to/libfprint/libfprint/drivers/
```

## 2. Register it

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

## 3. Build and install

```sh
meson setup build -Ddoc=false -Dintrospection=false
ninja -C build
sudo ninja -C build install
sudo ldconfig
sudo systemctl restart fprintd
```

The driver is now built into `libfprint-2.so`; `fprintd` picks it up
automatically.

## 4. udev rules

libfprint's own rules only cover the devices it knows about, so the sensor
needs a rule of its own:

```
SUBSYSTEM=="usb", ATTR{idVendor}=="04e8", ATTR{idProduct}=="7301", TAG+="uaccess"
```

See [`../contrib/99-samsung-7301.rules`](../contrib/99-samsung-7301.rules).

## Notes on the implementation

* **The state machine has to be created per activation.**  libfprint frees an
  `FpiSsm` once it completes, so the driver creates a new one in
  `s7301_dev_activate()` and a new one in `s7301_dev_open()`.

* **Command buffers need static storage.**  The USB transfers are
  asynchronous, so the command bytes must stay valid until the transfer
  completes; a compound literal inside a `switch` case does not.

* **The image read arms the bulk IN before the bulk OUT.**  See
  `s7301_img_after_prime()`.

* **`BZ3_THRESHOLD` is lowered to 15.**  The default of 40 rejects every
  genuine match, because the frames of this sensor are noisy.  A lower
  threshold means a higher false accept rate; use a strong password as well.

* **The driver implements `img_open`/`img_close`**, not `open`/`close`: the
  latter are provided by `FpImageDevice` and call the former.
