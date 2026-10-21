# Research log: is there an existing driver for `04e8:7301`?

Question asked: *"deep research on the internet, see if you can find a fingerprint
sensor driver"* for this device.

Answer: **No driver exists, and no reverse-engineering effort is known.** This
page records what was searched, so the work is not repeated.

## Places searched, and what they contain

| Source | Result |
|---|---|
| libfprint supported devices (dev list) | no `04e8:7301` |
| libfprint wiki, Unsupported Devices | no `04e8:7301` — only `04e8:730b` |
| `jedbillyb/linux-fingerprint-drivers` README (working + WIP drivers) | only `04e8:7305` (!586), `04e8:730b` (!556) |
| `jedbillyb/.../docs/unsupported-devices.md` (100-device gap map, with RE leads) | `04e8:7301` is *not even listed* |
| libfprint merge requests | `!586` samsung7305 (open), `!556` samsung730b (open); nothing for 7301 |
| Microsoft Update Catalog (POST search) | no `04e8:7301` driver entry |
| USB ID databases (rxtx.org, devicehunt, the-sz) | `04e8:7301` = "Fingerprint Device", Samsung VID. Metadata only, no protocol, no driver. |
| Samsung support download pages for `NP900X5T*` | 403 / no driver listings; drivers ship via Samsung Update only |
| ArchWiki Laptop/Samsung | several Galaxy Books listed "Fingerprint reader: No"; no Notebook 9 entry |

## Consequence

`04e8:7301` is undocumented. It is not a rebadge of anything that has been
reversed:

- It is **not** an FPC sensor. FPC parts enumerate under their own vendor ID
  `10a5:xxxx` (see the fwupd `fpc` plugin, which matches `USB\VID_10A5&...`).
  The press coverage of "FPC in a Notebook 9" refers to the **2017** generation;
  this 2018 unit reports Samsung VID with iManufacturer `Samsung`.
- It is **not** the `04e8:730b` part despite sharing interface class `ff/02/00`.
  730b is described as an *image* driver; its own author had to write a driver
  from scratch, which means the class match carries no protocol information.
- The borrowed `04e8:7305` transport was already tested against this device and
  timed out (see `PROTOCOL.md`).

## Where the protocol has to come from

There is no shortcut. The command set is only observable from the vendor driver
running on Windows, captured over USB. Two viable capture routes:

1. Windows in a VM with the sensor handed through, traffic recorded on the Linux
   side with `usbmon` — see `docs/CAPTURE.md`. This is the route Neodyme used for
   their Goodix sensor and it works for fingerprint readers.
2. Windows bare metal from a USB drive — see `docs/WINDOWS-CAPTURE.md`.

Both share the same risk: Neodyme's sensor spoke **TLS-PSK over USB**, so the
capture showed structure but not plaintext, and the analysis had to move to
driver-side debugging. We will not know which category this sensor falls into
until a capture exists.

## Descriptor (for anyone who finds this page)

```
bcdUSB 2.00   bDeviceClass 0   bMaxPacketSize0 8   bcdDevice 0x0000
idVendor 0x04e8  idProduct 0x7301
iManufacturer "Samsung"   iProduct "Fingerprint Device"   iSerial 0
Interface 0: class 0xff (vendor), subclass 0x02, protocol 0, 3 endpoints
  EP 0x01  Bulk OUT  64
  EP 0x82  Bulk IN   64
  EP 0x83  Interrupt IN 64, interval 10
Full speed (12Mbps), self powered, remote wakeup, 100mA
```

The layout is unremarkable and matches many vendor-protocol sensors; it does not
identify the silicon.
