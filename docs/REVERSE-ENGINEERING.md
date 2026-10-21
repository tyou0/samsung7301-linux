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
