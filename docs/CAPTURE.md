# Capturing the Windows driver's USB traffic

This is the blocking artifact. A capture turns guesswork into a checklist.

## What you need

- The laptop booted into Windows (the driver that makes the sensor work).
- [USBPcap](https://github.com/desowin/usbpcap/releases) installed, which also
  installs a capture interface for Wireshark.
- [Wireshark](https://www.wireshark.org/).

USBPcap must be installed with the root hub you will capture attached. On most
laptops the internal sensor is behind the first root hub; if the device does not
appear, capture "all root hubs".

## Procedure

1. Install USBPcap + Wireshark and reboot.
2. Start Wireshark, pick the `USBPcap1` (or the root hub that lists the
   fingerprint device) interface.
3. Begin capture, then immediately:
   1. Open **Settings → Accounts → Sign-in options → Fingerprint** and remove
      any existing fingerprints.
   2. Enroll one finger fully.
   3. Lock the machine (`Win+L`) and unlock with the fingerprint.
   4. Sign out/in once more.
4. Stop capture and save as `04e8-7301-enroll-verify.pcapng`.

## What to write down

- The exact Windows driver name/version
  (**Device Manager → Biometric devices → Properties → Driver**).
- Whether enrollment succeeded, and how many swipes it asked for.
- Laptop model string (`wmic csproduct get name`).

## Filtering the capture

In Wireshark, the sensor is usually `1.8.x` or similar under the root hub.
Useful display filters:

| Filter | Shows |
|---|---|
| `usb.idVendor == 0x04e8 && usb.idProduct == 0x7301` | all traffic to the sensor |
| `usb.transfer_type == 0x02` | control transfers (the `c3`/`ca`/`da` framing) |
| `usb.transfer_type == 0x03` | bulk transfers (commands + image data) |
| `usb.transfer_type == 0x01` | interrupt transfers (status/doorbell) |

Export the filtered packets as hex (`File → Export Packet Dissections → As Plain
Text`) and attach both the `.pcapng` and the text dump to an issue.

## Alternative: a Windows VM with USB passthrough

If you cannot dual-boot, a Windows VM can work *if* the hypervisor can pass the
USB device through (QEMU/KVM with `usb-host`, or VirtualBox USB filters). The
sensor must appear as `04e8:7301` inside the guest. Capture inside the guest with
USBPcap as above.

## Static analysis of the driver binary

A downloaded `.sys`/`.dll` is useful mainly for strings and IOCTL codes; it will
not hand you the USB command stream. Keep it as a cross-check:

```sh
# From the extracted installer directory
strings -el SamsungFingerprint.sys | less
grep -aob $'\x04\xe8\x01\x73' SamsungFingerprint.sys   # VID/PID little-endian
```

Look for the VID/PID, the heartbeat payload bytes, and one-byte command opcodes
from `PROTOCOL.md`. If the capture is available, prefer it for everything except
confirming calibration blobs.
