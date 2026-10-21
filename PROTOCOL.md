# Protocol notes — Samsung `04e8:7301`

> **These are hypotheses.** Nothing here has been confirmed against a capture of
> a `04e8:7301` device. The framing is borrowed from the `04e8:7305` driver
> (libfprint MR !586), which was reverse-engineered from Windows USB captures on
> a Samsung Notebook 7 Spin.

## Device comparison

| | `04e8:7301` (this sensor) | `04e8:7305` (known driver) |
|---|---|---|
| Laptop | Notebook 9 `NT900X5V` | Notebook 7 Spin `NP730QAA` |
| bcdDevice | `0x0000` | unknown |
| Class | vendor `ff/02/00` | vendor |
| Bulk OUT | EP `0x01` | EP `0x01` |
| Bulk IN | EP `0x02` (**`0x82`**) | EP `0x02` |
| Interrupt IN | EP `0x03` (**`0x83`**) | ? |
| MaxPacket | 64 | 64 |

The endpoint *addresses* match exactly once the direction bit is applied
(`0x82` = IN endpoint 2). This is encouraging but not proof of a shared command
set: vendors reuse USB interface layouts across sensors with different silicon.

## Transport hypothesis (from samsung7305.c)

Each command is bracketed by control transfers:

```
host -> device  control  0x40  bRequest 0xc3  wValue 0x0000  wIndex 0x0000
                          16-byte heartbeat payload (fixed)
host -> device  control  0x40  bRequest 0xca  wValue 0x0003  wIndex prime_len
                          (no data)
host -> device  bulk     EP 0x01  command bytes
device -> host  bulk     EP 0x82  prime_len bytes response
device -> host  control  0xc0  bRequest 0xda  wValue 0x0007  2 bytes (optional)
```

`0x40` = host→device, vendor, recipient=device.
`0xc0` = device→host, vendor, recipient=device.

The fixed heartbeat is byte-identical across every occurrence in the 7305
capture, which suggests it is a transport constant rather than device state:

```
c0 c6 2d 00 08 00 ff 01 01 01 00 00 00 00 00 00
```

## Commands (unconfirmed for 7301)

| Name | Bytes | Purpose |
|---|---|---|
| reset | `f8` | reset sensor |
| status | `1c 00` | read status register; response byte[1] = state |
| arm | `20` | arm for finger presence |
| capture setup | `54 50 18 00 38`, `a0 10 00`, `a8 0f 1a`, `64 0c`, `5c 0a` | configure capture |
| capture read | `d4 00 00 00 00` | read image strip |
| trigger | `c0` | trigger capture |

Status response byte[1]:

| Value | Meaning |
|---|---|
| `0x81` | finger present |
| `0x80` | idle |
| `0x20` | ready |

## Init sequence (7305)

Replayed verbatim from `INIT_SEQ` in `src/protocol.rs`. The **calibration blob**
(`98 03 ea bf 68 02 09 15 2a 7a de`) is the most suspect entry: on the 7305 it
was identical across two captures of the *same* unit, and the driver author notes
it may be per-unit. If the 7301 initialises but produces garbage images, suspect
this blob first.

## Frame assembly (7305, for reference)

The sensor streams strips rather than whole frames:

1. Capture setup, trigger, wait.
2. Read strip 1, then a "between strips" command `a8 0f 0a`.
3. Trigger/read strip 2, then strip 3.
4. A flat-field (`ff`) setup and a full frame read.
5. Strips are assembled into a `FpImage` via `fpi_frame_asmbl_ctx`.

Geometry constants from the 7305 capture: `54 50 18 00 38` and `54 00 c0 00 38`
suggest a 0x50×0x18 region and a 0xc0×0x38 frame. These are almost certainly
sensor-specific and must be re-derived for the 7301.

## What would confirm or refute this

A USBPcap trace of the Windows driver on a `04e8:7301` machine, covering:

1. driver load / device init,
2. one full enrollment,
3. one successful verification,
4. one failed verification.

With that trace, the work is mechanical: diff the observed control/bulk
sequences against this file, replace the constants, and replay them in Rust.
Without it, `samsung7301 status` is the best available test.
