# Reverse engineering the Samsung `04e8:7301` fingerprint sensor

This document records what was learned by reverse engineering the official
Samsung Windows driver for this exact laptop. It supersedes the earlier
guesses in `PROTOCOL.md` (which borrowed the sibling `04e8:7305` transport).

## 1. Obtaining the driver

The sensor has no Linux driver and no public protocol documentation. Samsung
ships a Windows driver per machine, delivered through the "Samsung Update"
backend. Reconstructing that backend let us download the driver directly.

### 1.1 Finding the backend

`SU.dll` (the Samsung Update client, from the Microsoft Store package
`SAMSUNGELECTRONICSCO.LTD.SamsungUpdate_3.0.110.0`) was decompiled with
`ilspycmd`. It revealed a REST/JSON API:

```
https://orcaservice.samsungmobile.com:10443/api/<Method>
```

and a legacy SOAP endpoint:

```
https://orcaservice.samsungmobile.com:10443/downloadcenter.asmx
```

### 1.2 Resolving this machine

The machine serial is readable from SMBIOS
(`/sys/class/dmi/id/product_serial`, root-only): `0XPN91AM100430`.

```
GET  api/ORCAModelName?XMLVERSION=3.0&SN=0XPN91AM100430
 ->  {"ModelName":"NT900X5V-A39A", "Result":"SUCC"}
```

The "Hardware Driver Management" call needs the right region/OS codes
(`OS` is `A3`..`A9`; `REGION` is a 3-letter code):

```
POST api/ORCAHDMInfo
  {"XMLVERSION":"3.0","SN":"0XPN91AM100430","REGION":"KOR",
   "OS":"A9","LANG":"ENG","CLIENTVERSION":"3.0.110.0",
   "ETC":"","TESTMODE":"NONE"}
 ->  {"HDM":{"ITEM":[
        {"ID":"MAXA702A05","FURL":"http://appmanager.samsungmobile.com/dl/bom/MAXA702A05.XML"},
        {"ID":"IMGA702A05-I001","FURL":".../IMGA702A05-I001.XML"}]}}
```

### 1.3 The driver manifest

The `MAXA702A05.XML` manifest lists every driver for this model. The
fingerprint entry:

```xml
<CISCode>BASW-A0951A28</CISCode>
<DisplayName>Fingerprint Sensor Driver(One) 3.23.0.15</DisplayName>
<InstallPath>BASW-A0951A\BASW-A0951A1P.ZIP</InstallPath>
<InstallFile>fpc_wbfsetup.exe</InstallFile>
<Keyword2>Fingerprint</Keyword2>
<Keyword3>FPC</Keyword3>
<FURL>http://orcaservice.samsungmobile.com/FileDownloader.aspx?FILENAME=BASW-A0951A1P.ZIP</FURL>
```

**`Keyword3: FPC`** is the vendor: **Fingerprint Cards AB**.

### 1.4 The package

```
http://orcaservice.samsungmobile.com/FileDownloader.aspx?FILENAME=BASW-A0951A1P.ZIP
 -> fpc_wbfsetup.exe, fpc_wbdi.dll, FpcTouchEngine.dll, fpc_wbdi_um_usb.inf
```

The INF confirms the binding:

```
Provider    = %FPC%                    ; Fingerprint Cards AB
DriverVer   = 05/17/2018,3.23.0.015
"FPC Fingerprint Reader" = FpcUsb_Install, USB\VID_04E8&PID_7301   <-- our device
"FPC Fingerprint Reader" = FpcUsb_Install, USB\VID_04E8&PID_7305
```

**The sensor is an FPC part.** Samsung rebadges FPC silicon under vendor ID
`04e8`. `FpcTouchEngine.dll` is the matching engine (image/template
processing); `fpc_wbdi.dll` is the USB transport driver.

## 2. The `fpc_wbdi.dll` transport

`fpc_wbdi.dll` is a native x86-64 UMDF driver, stripped, exporting only
`FxDriverEntryUm`. It contains a table of supported sensors and a full
command vocabulary.

### 2.1 Supported sensors

The driver recognises these FPC parts (string table at `.rdata:0x180022a20`):

```
FPC1020 FPC1021 FPC1022 FPC1025 FPC1035
FPC1140 FPC1145 FPC1150 FPC1155
FPC1260 FPC1265
```

Each is identified by a chip ID read over the bus (the driver's dispatch
compares `[ctx]` against): `0x3fc 0x3fd 0x474 0x47e 0x3fe 0x4ec`.

### 2.2 Command vocabulary

`fpc_wbdi.dll+0x18000fb5c` is a byte→name dispatcher (a binary search on
`cl`). The complete opcode table:

| Opcode | Name |
|--------|------|
| `0x10` | navdata |
| `0x1c` | read_interrupt_with_clear |
| `0x20` | finger_present_query |
| `0x24` | wait_for_finger_present |
| `0x28` | activate_sleep_mode |
| `0x2c` | activate_deep_sleep_mode |
| `0x30` | activatenav |
| `0x34` | activate_idle_mode |
| `0x3c` | clkbist |
| `0x4c` | imgsmplsetup |
| `0x54` | imgcapsize |
| `0x5c` | imagesetup |
| `0x64` | imagerd |
| `0x68` | sampledly |
| `0x6c` | pxlrstdly |
| `0x78` | tstcolpatternen |
| `0x7c` | clkbistresult |
| `0x80` | offsetdac |
| `0x8c` | fngrdriveconf |
| `0x90` | fngrdrivedly |
| `0x94` | osctrim |
| `0x98` | adcweighttbl |
| `0x9c` | adcsetup |
| `0xa0` | adcshiftgain |
| `0xa8` | pxlctrl |
| `0xac` | afdctrl |
| `0xb4` | capture_image |
| `0xb8` | read_image_data |
| `0xc8` | fngrpresentstatus |
| `0xd8` | fngrdetthresh |
| `0xdc` | fngrdetcntr |
| `0xe0` | fngrdownshrt |
| `0xe4` | fngrdownlong |
| `0xe8` | fngrdownhold |
| `0xec` | fngrareactrl |
| `0xf0` | afdcalibration |
| `0xf4` | afdstatus |
| `0xf8` | soft_reset |
| `0xfc` | hwid |

### 2.3 Wire framing

Two functions build bulk-OUT packets (`fpc_wbdi.dll+0x180008494` and
`+0x180008738`). Both write a **4-byte header** followed by payload:

```
0xc3 register-value form:  40 c3 00 00 <payload...>
0xca payload form:         40 ca <len_lo> <len_hi=00> <type_byte> <payload...>
```

- `40 c3` — command/register header, length implied by the call site.
- `40 ca` — bulk data header; bytes 3-4 are a **little-endian 16-bit payload
  length**, then a **type byte**, then the payload.

`0x180008738` takes a 16-bit payload word in `dx`, appends it after
`40 ca 03 00`, and issues a **3-byte** transfer (the type byte plus the 16-bit
word) — i.e. `40 ca 03 00 <type> <lo> <hi>`.

### 2.4 Chunked transfers

Large transfers use a fixed buffer of `0x6402` bytes (25600 payload + 2).
`fpc_wbdi.dll+0x18000a7e1`:

1. zero the 25602-byte buffer,
2. write a **type byte** at `buf[0]`,
3. write payload bytes after it,
4. send via the `40 ca` path with `len = payload_bytes + 1`,
5. read the reply into `[ctx+0x5f0]` via `0x1800099a4` (the `hw_usb_read`
   wrapper).

Transfers larger than 25600 bytes are split into chunks.

### 2.5 Timeouts

The read/write wrappers pass a packed timeout structure:

```
dword 0x002dc6c0   ; 3_000_000 us = 3 s
dword 0x01ff0008
qword 0x0000000000000101
```

Interrupt/mode polling uses `0xffffffffff676980` (a large relative timeout).

### 2.6 Register/configuration payload

`fpc_wbdi.dll+0x1800071d8` builds a register table for chip ID `0x4ec`
(FPC1022). It is a stream of **16-bit little-endian words** terminated by
`0x0100`, where the driver's `0x180007dd8` walker accumulates bytes and flushes
a `40 c3` packet each time it sees `0x0100`:

```
004c 0004 00ff 00ff 0011 0100
006c 0004 0100 0068 0013 0008 0008 0008 0008 0014 0014 0014 0014 0100
0080 0082 0015 0015 0015 0015 0009 0009 0009 0009 0100
0098 0003 00eb 003f 00b8 000a 0007 0011 0022 006a 005d 0100
008c 0000 0004
```

## 3. What this means for the Linux driver

The transport is now understood well enough to implement:

- It is **not** the `04e8:7305` control-transfer protocol; the 7305 code path
  in `protocol.rs` sent `0xc3`/`0xca` *vendor control* requests, but the real
  driver uses **bulk** transfers with a `40 c3` / `40 ca` header.
- A working sequence is: `soft_reset (0xf8)` → `hwid (0xfc)` → the register
  configuration table → `imagesetup (0x5c)` → `capture_image (0xb4)` →
  `read_image_data (0xb8)`.
- Finger detection polls `read_interrupt_with_clear (0x1c)` /
  `fngrpresentstatus (0xc8)`.

### Still unknown

- The exact **type byte** values used in the `40 ca` header per command.
- Whether the reply has a length/checksum trailer (the read wrapper parses a
  status but the payload layout is not yet fully traced).
- The precise `imagesetup` argument block for this specific sensor variant.

These are the remaining items a live `usbmon` capture would confirm. See
`docs/CAPTURE.md`.

## 4. Live probing results (2026-10-21)

With the Rust probe on the real `04e8:7301`, against the recovered framing:

| Test | Result |
|---|---|
| `info` (descriptors) | matches the Windows INF exactly |
| bulk OUT `fc` (raw opcode) | write succeeds |
| bulk IN read after command | returns **N zero bytes**, where N == length of the last OUT packet |
| bulk IN read with no command | returns the **stale tail of the previous OUT packet** |
| interrupt IN (0x83), any time | always times out |

**Interpretation.** The bulk IN endpoint currently behaves as a length-echo /
loopback: it returns exactly as many zero bytes as the last OUT transfer. This
means the device accepts bulk traffic but is **not in command mode**. A real
reply would be non-zero and would not track the OUT length.

The Windows driver therefore performs additional setup that static analysis has
not yet pinned down — most likely one or more of:

- a UMDF pipe configuration / `SetPipePolicy` with a specific max-transfer
  size and raw-IO mode,
- a `ResetPipe`/`AbortPipe` cycle before the first command,
- a control-transfer handshake,
- a wait for the interrupt endpoint (which stayed silent here because the
  device never entered command mode).

The command opcode table, the `40 c3`/`40 ca` framing, the register
configuration table, and the chunking rules are all still believed correct:
they come directly from the driver. What is missing is the **bring-up
handshake** before the first command.

### Next step

A `usbmon` capture of the Windows driver driving this sensor. See
`docs/CAPTURE.md`. The capture will show the bring-up transfers directly and
resolve the framing question definitively.

## 5. Bring-up handshake — recovered statically (2026-10-21, later)

A second pass over `fpc_wbdi.dll` used the PE x64 `.pdata` exception directory
to recover exact function boundaries, then walked the call graph from the two
framing helpers. That resolved the bring-up path **without needing a capture**.

### 5.1 The three transport primitives

All three live in `.text` and are reached from a small cluster of callers:

| Addr | Role | Wire effect |
|---|---|---|
| `0x180008494` | `hw_usb_register_write` | sends `40 c3 00 00` + a 2-byte register value |
| `0x180008738` | `hw_usb_bulk_write` | sends `40 ca <len_lo> <len_hi> 03 <payload>` |
| `0x18000a770` | `hw_usb_read` (logged as `hw_usb_read`) | reads a chunked reply |

`hw_usb_write` (`0x18000ad1c`, logged as `hw_usb_write`) is the command
sender. Its body makes the **ordering explicit**:

```
zeroes   a 0x6402-byte staging buffer at [dev+0x69f2]
memcpy   caller buffer -> staging buffer
call     0x180008494        ; register write   (40 c3 ...)
call     0x180008738        ; bulk data write  (40 ca <len> 03 <payload>)
```

So **every command is a register write followed by a bulk write** on the same
endpoints. There is no control-transfer prelude: the "register" write is itself
the first bulk packet.

`hw_usb_read` (`0x18000a770`) is the mirror image:

```
call     0x180008494                 ; arm / clear
loop:                               ; reads in <=0x6402-byte chunks
  read chunk -> copy into caller buffer
  decode reply by case on length (1,2,3,4,else) for logging
```

### 5.2 The bulk "type byte" is `0x03`

In `hw_usb_bulk_write` (`0x180008738` region) the header dword is built as
`0x0003ca40`:

```
40 ca 03 00
```

That is: `0x40`, `0xca`, then a 16-bit little-endian length, and the byte
immediately after the header is `0x03`. **`0x03` is the type byte for the
data path.** (The stale `type_byte` note in `PROTOCOL.md` is superseded.)

### 5.3 The power / mode-set command (single byte)

`0x180008274` is the power-mode setter. It takes a mode byte in `dl`, maps it
to a flag, and then calls `hw_usb_write` with **length 1** — a one-byte
command:

| Mode byte | Flag applied |
|---|---|
| `0x20` | `0x80` |
| `0x34` | `0x80` |
| `0x98` | `0xff` |
| `0xc0` | (ack check) |

After sending it, it polls the reply byte up to 30 times
(`call 0x18000a758` → `0x180008e3c`, a 7-unit read) and treats
**`0xc0` in the reply as success**.

So the first thing the driver does to the device is send a single byte
(`0x20` / `0x34` / `0x98`) and wait for `0xc0`.

### 5.4 The chip-config command

`0x180007c84` builds the per-chip configuration command. It compares the chip
ID at `[ctx]` against `0x4ec` (FPC1022):

```
chip == 0x4ec:  buf = a8 93|00 0f <byte from table 0x61412101>   (len 4)
otherwise:      buf = a8 0f <byte from table 0xa1a0e1e>          (len 3)
```

then `call 0x180008470` → `hw_usb_write`. `0xa8` is `pxlctrl` in the opcode
table; the following bytes are its arguments.

### 5.5 The bring-up state machine

The D0-entry routine near `0x18000b3a4` runs the sequence:

1. read `[ctx+0x90]` / `[ctx+0x94]` (image cols/rows) and compute `cols*rows`;
2. build a 38-byte (`0x26`) request block;
3. `call 0x180003284` — the command dispatcher;
4. inside it, `call 0x1800073e8` — **status poll** — up to 10 times with a
   10 ms delay (`mov ecx, 0xa; call 0x180007f64`), succeeding once the state
   code is `< 6`;
5. `call 0x180003ae0` — send the 3-byte prefix `00 40 eb`;
6. `call 0x18000782c` — write data;
7. `call 0x18000592c` — read the reply; result stored at `[ctx+0x518]` /
   `[ctx+0x51c]`.

The status poll `0x1800073e8` reads a 4-byte register (`dl=0xd4`, size 4 via
`0x18000811c`) and decodes a **16-bit little-endian status word**
(`hi*0x100 + lo`), splitting it into individual bits (bit 0 … bit 11) to derive
the state. This is the interrupt/status register.

### 5.6 Consequence for the probe

The reason the earlier probe saw "length echo" is that it sent raw opcodes
(`fc`, etc.) as the **very first** transfer. The device had never received the
power/mode byte, so it never left its default state and echoed the bulk
length. The correct order is:

```
1. power/mode byte  (0x20 | 0x34 | 0x98)      -> wait for 0xc0
2. chip config cmd  (a8 0f <arg>  /  a8 93 0f <arg>)
3. status poll      (register read 0xd4, len 4) until state < 6
4. then normal commands, each as reg-write + bulk-write (type 0x03)
```

Step 1 (`0x20` / `0x34` / `0x98`) plus waiting for `0xc0` is the missing
handshake.

## 6. CORRECTION — the command channel is control transfers

Section 5 is **superseded**. A later pass found that the register/bulk write
helpers are only one half of the picture. The other half is a **USB
control-transfer** helper at `0x180008964` that the register/bulk path never
calls. Reading the two thin wrappers around it settles the protocol.

### 6.1 The control-transfer primitive

`0x180008964` builds an 8-byte USB setup packet:

```
neg  r12d                    ; r12d = direction flag (edx)
sbb  cl, cl                  ; 0xff if flag != 0 else 0x00
and  cl, 0x80                ; 0x80 (device -> host)  or 0
and  cl, 0xdf
or   cl, 0x40                ; -> bmRequestType = 0xC0 (read) | 0x40 (write)
mov  byte [rbp-0x28], cl     ; bmRequestType
mov  byte [rbp-0x27], r8b    ; bRequest
mov  word [rbp-0x26], r9w    ; wValue
mov  word [rbp-0x24], ax     ; wIndex
```

It is a **vendor control transfer** (WDF
`WdfUsbTargetDeviceSendControlTransferSynchronously`), not a bulk transfer.

### 6.2 The two wrappers

| Addr | Direction | bmRequestType | bRequest | wValue | wIndex | wLength |
|---|---|---|---|---|---|---|
| `0x180008e3c` | read | `0xC0` | `0xda` | `dl` | `0` | `2` |
| `0x180008eac` | write | `0x40` | `0xdb` | `dl` | `r8w` | `0` |

So the **command channel** is:

```
control OUT:  40 db <wValue_lo> <wValue_hi> <wIndex_lo> <wIndex_hi> 00 00   (no data)
control IN :  c0 da <wValue_lo> <wValue_hi> 00 00 02 00   -> 2 bytes
control IN :  c0 da <wValue_lo> <wValue_hi> 00 00 04 00   -> 4 bytes (status)
```

This is the FPC control-request family (`bRequest 0xda`/`0xdb`) — the same
shape the `fpcmoc` driver uses, but with different request numbers and no
Match-on-Chip semantics.

### 6.3 The actual bring-up commands

Every chip-specific init call site passes `wValue` in `dl` and `wIndex` in
`r8b`. Recovered sites:

| Site | chip gate | wValue | wIndex | Meaning |
|---|---|---|---|---|
| `0x18000bb37` | `[ctx]==0x474` | `0x0b` | `0` | chip init |
| `0x18000dab3` | `[ctx+0x6c]==0x474` | `0x0b` | `0` | chip init |
| `0x18000e52b` | `[ctx+0x6c]==0x474` | `0x0b` | `0` | chip init |
| `0x180008004` | `[ctx+0x6c]==0x474` | `0x0b` | `1` | chip init (idx 1) |
| `0x1800042ba` | — | `0x06` | `0` | re-init |
| `0x1800042d0` | — | `0x06` | `1` | re-init follow-up |
| `0x180004457` | — | `0x06` | `0` | re-init |
| `0x180004474` | — | `0x06` | `1` | re-init follow-up |

The read side (`0xda`) is used for the mode acknowledgement (`0x18000a758` →
`0x180008e3c`, 2 bytes) and for the status register (`0x18000811c` → the
`0x18000a770` reader, 4 bytes, `wValue=0xd4`).

### 6.4 What the register/bulk frames really are

`40 c3 ...` / `40 ca ...` are **bulk** transfers on the OUT pipe
(`[dev+0x5b8]`), used by the image/streaming path. They are not the command
channel. `hw_usb_write` (`0x18000ad1c`) is the bulk-data sender; the
`0xda`/`0xdb` control requests are how commands are issued.

### 6.5 Corrected bring-up order

```
1. control OUT  bRequest=0xdb, wValue=0x0b, wIndex=0     ; chip init
   (and, for some parts, wIndex=1)
2. control IN   bRequest=0xda, wValue=?                  ; read the ack/status
3. bulk path for image data
```

This supersedes §5. The exact `wValue` depends on the chip; `0x0b` is the value
used for `0x474`, and the driver's chip tables list `0x4ec` (FPC1022) for this
laptop.

---

## 7. The final protocol (2026-10-21)

Sections 1–6 describe the reverse engineering of the Windows driver.  This
section records what was *verified on the hardware* afterwards, and where the
earlier conclusions had to be corrected.

### 7.1 The transport

The sensor is a **FPC1021** (revision `0x1f`), 160x160 pixels.  The
`hwid` command answers `00 02 1f` and `imgcapsize` reports `0xa0 x 0xa0`,
which matches the `0x3fd` (FPC1021) entry of the driver's chip table.

Every command exchange is:

```
1. CTRL-OUT  bRequest=0xc3  wValue=0x0000 wIndex=0x0000, 16 byte heartbeat
2. CTRL-OUT  bRequest=0xca  wValue=0x0003 wIndex=<reply length>
3. BULK-OUT  <command bytes>                             (EP 0x01)
4. BULK-IN   <reply length> bytes                       (EP 0x82)
5. CTRL-IN   bRequest=0xda  wValue=0x0007, 2 bytes       (doorbell, optional)
```

The **reply length always equals the length of the command**.  The `0xca`
request tells the sensor how many bytes to stage on the bulk IN pipe.

The session is opened with four `0xdb` writes: `wValue` 0x0006 and 0x000b,
each with `wIndex` 0 and 1.  This part of the sensor **stalls the `0x000b`
writes**; the `0x0006` pair is the one that matters.

The `0xc3` heartbeat payload is fixed:

```
c0 c6 2d 00 08 00 ff 01 01 01 00 00 00 00 00 00
```

This transport was cross checked against the USB capture of the same driver
family published in libfprint merge request !586 (`04e8:7305`); the capture
matches byte for byte.

### 7.2 The image read has to be concurrent

The image is transferred with a `0xc4` command padded to the length of the
transfer (25602 bytes for a 160x160 frame plus a two byte header).  The sensor
**starts streaming the frame while the host is still writing that padded
command**, so the bulk IN has to be submitted *before* the bulk OUT.  With a
sequential read only the last few hundred bytes arrive; with the read armed
first the whole frame arrives.

The same applies to the Rust implementation, which uses raw libusb
asynchronous transfers for this.

### 7.3 The per-chip configuration table

The Windows driver builds a table of 16 bit words and walks it into
"register, value, ..." frames (`0x180007dd8`); the word `0x0100` ends a
frame and the low byte of every other word is appended to it.  For the
FPC1021 (`0x3fd`) the table is:

```
9c 55 40 00 2d 24
6c 29 00 90 20
68 31 31 3d 3d
98 03 eb 3f b8 0a 70 11 22 6a 5d
8c 12 00 5c b0
d8 20 00
```

followed by the `dc 00 0a` command that `0x180005e24` sends for this chip.

The equivalent sequence for the sibling `04e8:7305` (chip `0x474`) is what
the capture in !586 shows, which confirms the interpretation.

### 7.4 The analogue front end has to be retuned

The driver's settings are for revision `0x10`.  On revision `0x1f` they
saturate the sensor: the frame is a flat field at 246/255 with a handful of
darker pixels, and only the first eight rows of a frame carry any data at
all.

Two registers matter, and both were found by sweeping them with a finger on
the sensor and measuring the standard deviation of the frame:

| command | driver's value | working value | effect |
|---|---|---|---|
| `a0 00 00` (`adcshiftgain`) | `a0 0a 02` | `a0 00 00` | gain 0 instead of 0x10 |
| `9c 55 40 00 2d 24` (`adcsetup`) | `9c 55 40 00 2d 24` | `9c 55 40 00 08 24` | the fourth argument |

With those, a frame has a standard deviation of ~30 instead of ~2, and every
row carries data.

`64 12` (`imagerd`) is the readout mode with the best contrast.

### 7.5 What still does not work

The frames are not a clean fingerprint image: they are a mostly flat field
with a strong per-column pattern and no periodicity in the ridge direction.
The capacitive finger detection works (status `0x81`), so the finger is on
the sensor, but the optical image does not show the ridges.

Enrolment and verification still work with those frames — libfprint's minutiae
detector finds enough points in them — but the bozorth3 match threshold has
to be lowered from 40 to 15, which raises the false accept rate.

The most likely explanation is that the sensor needs a per-revision
calibration table that the 2018 driver does not contain: the driver's own chip
detection (`fpc_wbdi.dll+0x180005c78`) does not know the `0x1f` revision
either, it only knows `0x00`–`0x0f` and `0x10` for the `0x02` family.
