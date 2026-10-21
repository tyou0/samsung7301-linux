# Capturing the 04e8:7301 USB traffic

This supersedes the earlier bare-metal plan in `WINDOWS-CAPTURE.md`. Research
into how comparable sensors were reversed (notably Neodyme's Goodix writeup,
<https://neodyme.io/en/blog/fingerprint_reversing>) shows the VM route works and
is less disruptive: the sensor is passed through to a Windows guest, and the
traffic is captured **on the Linux host** with `usbmon`. No USBPcap needed in the
guest, and startup packets before the driver loads are visible.

## Why this route

The sensor is a USB device on the host's xHCI controller. A guest reaches it only
through USB passthrough; a virtual disk does not help. Passthrough has been
demonstrated to work for fingerprint readers, so it is worth trying before
touching the internal disk.

The device is a good passthrough candidate:

- Port 008 on Bus 001, `Driver=[none]`, vendor-specific class, not bound by any
  kernel driver — nothing to fight over.

## Prerequisites (already done on this machine)

```sh
sudo pacman -S qemu-desktop edk2-ovmf swtpm usbredir
sudo modprobe usbmon
sudo mount -t debugfs none /sys/kernel/debug
```

`qemu-system-x86_64` 11.1.2 is installed, `usbmon` is loaded, and the capture
nodes under `/sys/kernel/debug/usb/usbmon/` exist (`1u` for bus 1).

## Step 1 — Install Windows in a VM

Create a disk and install Windows 10 (lighter than 11, captures the same):

```sh
qemu-img create -f qcow2 ~/vm/windows.qcow2 50G
```

Boot it with OVMF and the Windows ISO attached. Give it 3–4 GB RAM (this host has
7.7 GB total) and 2 vCPUs. Enable an EHCI controller so the sensor can attach to
a USB 2.0 bus.

Rough invocation to adapt:

```sh
qemu-system-x86_64 \
  -enable-kvm -m 3584 -smp 2 \
  -machine q35 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
  -drive if=pflash,format=raw,file=$HOME/vm/OVMF_VARS.fd \
  -device usb-ehci,id=ehci \
  -device usb-host,bus=ehci.0,hostport=1.8 \
  -drive file=$HOME/vm/windows.qcow2,if=virtio \
  -cdrom /path/to/windows.iso \
  -boot d -display gtk
```

`hostport=1.8` pins the sensor by physical port (bus 1, port 8) so it survives
replugs. Check it enumerated in the guest with Device Manager.

## Step 2 — Install the fingerprint driver in the guest

Windows Update, or Samsung Update from the Microsoft Store, should offer the
driver. If the installer refuses because SMBIOS does not look like a Samsung
machine, that is the known limitation of the VM route — fall back to
`WINDOWS-CAPTURE.md` (bare metal).

Confirm it works: **Settings → Accounts → Sign-in options → Fingerprint** should
permit enrollment. Note the driver name and version from Device Manager.

## Step 3 — Capture with usbmon on the host

With the VM running and the driver working, capture bus 1 on the **host**:

```sh
sudo cat /sys/kernel/debug/usb/usbmon/1u > ~/samsung7301-linux/captures/bus1-raw.txt
```

Or use Wireshark directly: it lists `usbmon1` as a capture interface.

Then, in the guest: remove fingerprints, enroll one finger, lock/unlock once.

Stop the capture (Ctrl-C).

## Step 4 — Reduce the capture

The raw `usbmon` text is noisy. Filter to the sensor by its bus/device address
and extract the transfers. Keep both the raw file and a filtered summary.

Useful Wireshark display filters once converted:

| Filter | Shows |
|---|---|
| `usb.idVendor == 0x04e8 && usb.idProduct == 0x7301` | sensor traffic |
| `usb.transfer_type == 0x02` | control transfers |
| `usb.transfer_type == 0x03` | bulk transfers |
| `usb.transfer_type == 0x01` | interrupt transfers |

## The difficulty to watch for

Neodyme's Goodix sensor used **TLS-PSK over USB**. The image payload was
encrypted, and the pre-shared key was protected with a whitebox cipher, so a
passive capture revealed structure but not plaintext. If the Samsung sensor does
the same, a capture alone is not enough:

- look for high-entropy payloads of constant size (image data),
- look for handshake-shaped exchanges right after init,
- treat driver-side debugging (x64dbg against a UMDF driver) as the escalation.

Signs the protocol is *not* encrypted: short commands with obviously structured,
low-entropy replies, and repeated identical byte sequences.

## Fallbacks, in order

1. This VM route.
2. Bare metal Windows from a USB SSD (`WINDOWS-CAPTURE.md`).
3. Repartition the internal disk for a Windows install (riskiest).
