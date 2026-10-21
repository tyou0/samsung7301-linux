# Getting a Windows USB capture of the 04e8:7301 sensor

This is the blocking step. Everything else is done: the Rust probe exists, the
device is accessible without sudo, and the virtualization/imaging tools are
installed. What remains is a recording of the working Windows driver.

## Why this way

The sensor ignores every non-Samsung driver — we proved that (`samsung7301
status` times out; the device is otherwise silent). Its command set is
undocumented. The only way to learn it is to observe the driver that works.

Windows **bare metal** is chosen over a VM because Samsung's driver installer
checks SMBIOS/chassis identity and frequently refuses to bind on a "QEMU
Standard PC". Booting Windows from an external drive leaves the sensor in its
native slot, so the driver installs normally.

## Requirements

- One USB 3.0 drive (SSD preferred, flash works), **64 GB minimum**.
  A slow stick makes Windows painful but still works.
- A Windows ISO:
  <https://www.microsoft.com/software-download/windows10>
  (Windows 10 is lighter than 11 and captures identically.)
- This laptop, and about 2–3 hours.

## Step 1 — Backup anything on the USB drive

It will be wiped.

```sh
lsblk -o NAME,SIZE,TRAN,RM,MODEL   # identify your drive, e.g. /dev/sda
```

## Step 2 — Make the drive bootable

Two routes.

### Route A — Ventoy (easiest, keeps the drive reusable)

Ventoy lets you drop the ISO onto the drive as a file; it boots it directly.

```sh
yay -S ventoy
sudo ventoy2disk.sh -i /dev/sdX     # replace sdX with your drive
```

Then copy the Windows ISO onto the Ventoy partition and boot it. During Windows
Setup you can choose to install to the same drive, but note Setup may refuse to
install to the USB it booted from — in that case use Route B.

### Route B — Apply the image directly (true Windows-to-Go)

More manual, but installs Windows *onto* the drive so it runs natively.

```sh
# 1. Partition and format the drive as NTFS
sudo parted /dev/sdX --script mklabel gpt
sudo parted /dev/sdX --script mkpart ESP fat32 1MiB 513MiB
sudo parted /dev/sdX --script set 1 esp on
sudo parted /dev/sdX --script mkpart primary ntfs 513MiB 100%
sudo mkfs.vfat -F32 /dev/sdX1
sudo mkfs.ntfs -f /dev/sdX2

# 2. Mount the ISO and find the install image
sudo mount -o loop,ro /path/to/win10.iso /mnt/iso
wiminfo /mnt/iso/sources/install.wim      # list available editions

# 3. Apply the image to the NTFS partition
sudo mount /dev/sdX2 /mnt/target
sudo wimapply /mnt/iso/sources/install.wim 1 /mnt/target   # 1 = first edition

# 4. Make it bootable (needs a Windows bcdboot, or a template BCD)
#    Simplest reliable way: let Windows Setup repair the boot record on
#    first boot, or use the Ventoy bootmgr path.
```

Route B's final bootloader step is the fiddly part. If it fights you, use
**Route A (Ventoy)** and install Windows onto your internal disk instead — you
have 212 GB free and the Linux install is fully encrypted, so a shrunken
partition is possible but riskier. Ventoy + install-to-internal is the lowest
friction overall if you are willing to repartition.

## Step 3 — Boot Windows and install the fingerprint driver

1. Enter the firmware boot menu (usually `F10` or `F12` on Samsung; try `Esc`).
2. Boot the USB drive.
3. Install Windows (skip the product key; "I don't have internet" to use a local
   account).
4. Once booted, install **Samsung Update** from the Microsoft Store, or run
   Windows Update, and let it install the fingerprint driver.
5. Confirm it works: **Settings → Accounts → Sign-in options → Fingerprint**
   should let you enroll a finger.
6. Note the driver details: **Device Manager → Biometric devices → Properties →
   Driver** (name, provider, version). Write them down.

## Step 4 — Capture the traffic

1. Install [USBPcap](https://github.com/desowin/usbpcap/releases) and
   [Wireshark](https://www.wireshark.org/). Reboot.
2. Start Wireshark; pick the `USBPcap` interface for the root hub that lists the
   fingerprint device. If unsure, capture all root hubs.
3. Begin capture, then:
   1. Remove any enrolled fingerprints.
   2. Enroll one finger completely.
   3. Lock (`Win+L`) and unlock with the fingerprint.
   4. Sign out and back in once.
4. Stop capture, save as `04e8-7301-enroll-verify.pcapng`.

Useful Wireshark filters:

| Filter | Shows |
|---|---|
| `usb.idVendor == 0x04e8 && usb.idProduct == 0x7301` | all sensor traffic |
| `usb.transfer_type == 0x02` | control transfers |
| `usb.transfer_type == 0x03` | bulk transfers (commands + image) |
| `usb.transfer_type == 0x01` | interrupt transfers |

## Step 5 — Bring it back

Copy `04e8-7301-enroll-verify.pcapng` to Linux somewhere like
`~/samsung7301-linux/captures/`. Also record:

- the driver name/provider/version from Step 3.6,
- whether enrollment succeeded and how many swipes it took,
- the exact laptop model string.

Then the analysis begins: diff the observed framing against `PROTOCOL.md`,
replace the borrowed constants, and replay the sequence in Rust.

## What QEMU can also do (if you do use the VM)

`qemu-system-x86_64`'s `usb-host` device has a `pcap=` option that writes USB
traffic to a pcap from the **Linux** side — no USBPcap needed in the guest:

```sh
-device usb-host,hostport=1.8,pcap=/tmp/sensor.pcap
```

The sensor is on bus 1, port 8 (`hostport=1.8`), interface driver `[none]`, so
it is free to hand over. This is the fallback route, with the caveat that the
Samsung driver may not bind inside the VM.
