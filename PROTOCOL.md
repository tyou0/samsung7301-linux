# Protocol of the Samsung `04e8:7301` fingerprint sensor

> This is the *verified* protocol, as implemented in `src/protocol.rs` and
> `contrib/samsung7301.c`.  For the reverse engineering account (and the
> earlier, partly wrong conclusions it replaces) see
> [`docs/REVERSE-ENGINEERING.md`](docs/REVERSE-ENGINEERING.md).

## Transport

Every command exchange is bracketed by vendor control requests on EP0:

```
1. CTRL-OUT  bRequest=0xc3  wValue=0x0000 wIndex=0x0000, 16 byte heartbeat
2. CTRL-OUT  bRequest=0xca  wValue=0x0003 wIndex=<reply length>
3. BULK-OUT  <command bytes>                             (EP 0x01)
4. BULK-IN   <reply length> bytes                       (EP 0x82)
5. CTRL-IN   bRequest=0xda  wValue=0x0007, 2 bytes       (doorbell, optional)
```

The heartbeat payload is fixed:

```
c0 c6 2d 00 08 00 ff 01 01 01 00 00 00 00 00 00
```

The **reply length of a command is always the length of the command**.

The doorbell read (step 5) is only used by the opcodes `0x20` (arm),
`0x28` (init complete), `0xc0` (trigger) and `0xf8` (reset).

## Session

Four vendor control writes open the session:

```
CTRL-OUT bRequest=0xdb wValue=0x0006 wIndex=0
CTRL-OUT bRequest=0xdb wValue=0x0006 wIndex=1
CTRL-OUT bRequest=0xdb wValue=0x000b wIndex=0
CTRL-OUT bRequest=0xdb wValue=0x000b wIndex=1
```

This sensor stalls the two `0x000b` writes; the `0x0006` pair is the one
that matters.

## Bring-up sequence

```
f8                     reset (doorbell)
1c 00                  status
1c 00                  status
fc 00 00               hardware id -> 00 02 1f
9c 55 40 00 2d 24      adcsetup
6c 29 00 90 20         pxlrstdly
68 31 31 3d 3d         sampledly
98 03 eb 3f b8 0a 70 11 22 6a 5d   adcweighttbl (calibration)
8c 12 00 5c b0         fngrdriveconf
d8 20 00               fngrdetthresh
dc 00 0a               fngrdetcntr
5c 0b                  imagesetup
a8 0f 0a               pxlctrl
a0 0a 02               adcshiftgain
28                     init complete (doorbell)
```

Followed by the tuning of the analogue front end for this chip revision:

```
a0 00 00               adcshiftgain, gain 0
9c 55 40 00 08 24      adcsetup
64 12                  imagerd
```

## Capture sequence

```
20                     arm (doorbell)
1c 00                  status; 0x81 means "finger present"
d4 00 00 00 00         capacitive read
54 00 a0 00 a0         capture geometry: 160 rows, 160 columns
c0                     trigger (doorbell)
c4 <25602 bytes>       read the image (padded command, concurrent transfer)
```

The image reply starts with a two byte header, the rest is 160x160 pixels of
one byte each.

## Status codes

The `1c 00` reply is two bytes; the second one is the state:

| value | meaning |
|---|---|
| `0x00` | idle |
| `0x01` | finger detected |
| `0x81` | finger present |
| `0xff` | not initialised yet |

---

# Protocol notes — Samsung `04e8:7301`

> **Status: derived from the vendor driver, not yet confirmed over the wire.**
> The 7301 was previously thought to share the `04e8:7305` transport; that is
> now known to be wrong. This file describes the protocol recovered by
> disassembling the official Samsung Windows driver for this exact laptop
> (`fpc_wbdi.dll`, version 3.23.0.15). See `docs/REVERSE-ENGINEERING.md` for
> how the driver was obtained and analysed.

## Device identity

The sensor is an **FPC (Fingerprint Cards AB)** part. Samsung's own INF,
`fpc_wbdi_um_usb.inf`, binds it:

```
Provider    = %FPC%                    ; Fingerprint Cards AB
DriverVer   = 05/17/2018,3.23.0.015
"FPC Fingerprint Reader" = FpcUsb_Install, USB\VID_04E8&PID_7301
"FPC Fingerprint Reader" = FpcUsb_Install, USB\VID_04E8&PID_7305
```

The same driver covers `04e8:7301` and `04e8:7305`.

| | `04e8:7301` (this sensor) |
|---|---|
| Laptop | Notebook 9 `NT900X5V-A39A` |
| bcdDevice | `0x0000` |
| Class | vendor `ff/02/00` |
| Bulk OUT | EP `0x01` |
| Bulk IN | EP `0x82` |
| Interrupt IN | EP `0x83` |
| MaxPacket | 64 |
| Speed | full speed (12 Mbps) |

## Supported FPC sensors

`fpc_wbdi.dll` recognises these parts:

```
FPC1020 FPC1021 FPC1022 FPC1025 FPC1035 FPC1140 FPC1145 FPC1150 FPC1155 FPC1260 FPC1265
```

## Command set

`fpc_wbdi.dll+0x18000fb5c` is a byte→name dispatcher over the command opcode.
The full table:

| Opcode | Name | Opcode | Name |
|--------|------|--------|------|
| `0x10` | navdata | `0x90` | fngrdrivedly |
| `0x1c` | read_interrupt_with_clear | `0x94` | osctrim |
| `0x20` | finger_present_query | `0x98` | adcweighttbl |
| `0x24` | wait_for_finger_present | `0x9c` | adcsetup |
| `0x28` | activate_sleep_mode | `0xa0` | adcshiftgain |
| `0x2c` | activate_deep_sleep_mode | `0xa8` | pxlctrl |
| `0x30` | activatenav | `0xac` | afdctrl |
| `0x34` | activate_idle_mode | `0xb4` | capture_image |
| `0x3c` | clkbist | `0xb8` | read_image_data |
| `0x4c` | imgsmplsetup | `0xc8` | fngrpresentstatus |
| `0x54` | imgcapsize | `0xd8` | fngrdetthresh |
| `0x5c` | imagesetup | `0xdc` | fngrdetcntr |
| `0x64` | imagerd | `0xe0` | fngrdownshrt |
| `0x68` | sampledly | `0xe4` | fngrdownlong |
| `0x6c` | pxlrstdly | `0xe8` | fngrdownhold |
| `0x78` | tstcolpatternen | `0xec` | fngrareactrl |
| `0x7c` | clkbistresult | `0xf0` | afdcalibration |
| `0x80` | offsetdac | `0xf4` | afdstatus |
| `0x8c` | fngrdriveconf | `0xf8` | soft_reset |
| | | `0xfc` | hwid |

## Wire framing

Commands are sent as **bulk** transfers (not vendor control requests — the
earlier `0xc3`/`0xca` control-transfer hypothesis was wrong). Each packet is a
4-byte header followed by a payload:

```
register/value form:  40 c3 00 00 <payload...>
bulk data form:       40 ca <len_lo> <len_hi> <type_byte> <payload...>
```

- `40 c3` — command/register header.
- `40 ca` — bulk data header with a little-endian 16-bit payload length and a
  type byte before the payload.

The small-packet sender (`fpc_wbdi.dll+0x180008738`) builds
`40 ca 03 00 <type> <lo> <hi>` and issues a 3-byte transfer.

### Chunking

Large transfers use a 25602-byte buffer (25600 payload + 2). A type byte is
written first, then the payload, and the size sent is `payload_len + 1`.
Transfers larger than 25600 bytes are split into chunks.

### Timeouts

The read/write wrappers pass a timeout structure of 3 000 000 µs (3 s):

```
dword 0x002dc6c0   ; 3_000_000 us
dword 0x01ff0008
qword 0x0000000000000101
```

## Register configuration

`fpc_wbdi.dll+0x1800071d8` builds a register table for chip ID `0x4ec`
(FPC1022). It is a stream of 16-bit little-endian words terminated by `0x0100`.
The driver's parser (`0x180007dd8`) accumulates bytes and flushes a `40 c3`
packet whenever it sees `0x0100`:

```
004c 0004 00ff 00ff 0011 0100
006c 0004 0100 0068 0013 0008 0008 0008 0008 0014 0014 0014 0014 0100
0080 0082 0015 0015 0015 0015 0009 0009 0009 0009 0100
0098 0003 00eb 003f 00b8 000a 0007 0011 0022 006a 005d 0100
008c 0000 0004
```

## Operation sequence (inferred)

```
soft_reset (0xf8)
hwid (0xfc)
register configuration table
imagesetup (0x5c)
capture_image (0xb4)
read_image_data (0xb8)
```

Finger detection polls `read_interrupt_with_clear (0x1c)` or
`fngrpresentstatus (0xc8)`.

## Still unknown

- The exact **type byte** values in the `40 ca` header, per command.
- Whether the reply carries a length/checksum trailer.
- The precise `imagesetup` argument block for this sensor variant.

A live `usbmon` capture of the Windows driver (or the working Linux driver
once written) will resolve these. See `docs/CAPTURE.md`.
