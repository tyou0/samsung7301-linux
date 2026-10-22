#!/usr/bin/env python3
"""Probe the finger detection registers of the Samsung 04e8:7301 sensor.

Temporary research tool for the "no finger on the sensor but three bogus
captures" problem.  It compares the finger detection register profiles and
prints, for every round, the interrupt register, the capacitive zone mask
and the error status - never an image.

Not a read only probe in the strict sense: it writes volatile sensor
registers (reset, bring-up and profile values), which are lost when the
sensor is reset or unplugged.  Apart from that it stores nothing:

* no image is read (the 0xc4 image command is never sent), no fingerprint
  template is written and no file is created,
* no fingerprint is enrolled, verified or deleted; the tool does not talk to
  libfprint or fprintd at all.

Requirements:

* USB access to the reader.  The udev rule in
  ``contrib/99-samsung-7301.rules`` (``TAG+="uaccess"``) grants it to the
  user sitting at the machine, so the raw USB node has to be writable for
  this user.
* fprintd must be stopped: it holds the reader exclusively.

      sudo systemctl stop fprintd      # before the probe
      sudo systemctl start fprintd     # afterwards

Usage:

    python3 tools/probe-finger-profiles.py --count 3 --delay 0.05
"""
import argparse
import sys
import time

import usb.core
import usb.util

VENDOR_ID = 0x04E8
PRODUCT_ID = 0x7301
INTERFACE = 0
EP_OUT = 0x01
EP_IN = 0x82
TIMEOUT_MS = 2000

# Fixed heartbeat that precedes every command exchange (see PROTOCOL.md).
HEARTBEAT = bytes.fromhex("c0c62d000800ff010101000000000000")

# Interrupt flags of the 0x1c reply, as used by the libfprint driver.
IRQ_FINGER_DOWN = 0x01
IRQ_ERROR = 0x04
IRQ_COMMAND_DONE = 0x80
IRQ_REBOOT = 0xFF

# The corrected bring-up sequence.  0x28 (sleep) is deliberately not sent:
# the sensor has to stay awake for the detection to work.
INIT_COMMANDS = (
    "9c5540002d24", "6c29", "9032", "68313131313d3d3d3d",
    "9803eb3fb80a0711226a5d", "8c12", "5c0b", "d820", "dc000a",
    "a80f0a", "a00a02",
)

# Register profiles.  "init" is the corrected bring-up sequence, "extra" the
# values that override it afterwards.
PROFILES = {
    "windows": {"init": True, "extra": ()},
    "tuned": {"init": True, "extra": ("a00000", "9c5540000824", "6412")},
    "factory": {"init": False, "extra": ("a00a02", "a80f0a", "8c12")},
}

# Only these three profiles are run.
PROFILE_ORDER = ("windows", "tuned", "factory")


class ProbeError(Exception):
    """A failed USB exchange."""


def exchange(dev, command, doorbell=False):
    """Run one command exchange and return the reply bytes.

    The framing is the one of the libfprint driver: heartbeat control write,
    the 0xca control write that primes the reply length, the bulk write of
    the command, the bulk read of the reply and an optional doorbell read.
    The reply always has the length of the command.
    """
    command = bytes.fromhex(command)
    dev.ctrl_transfer(0x40, 0xC3, 0x0000, 0x0000, HEARTBEAT, TIMEOUT_MS)
    dev.ctrl_transfer(0x40, 0xCA, 0x0003, len(command), b"", TIMEOUT_MS)
    written = dev.write(EP_OUT, command, TIMEOUT_MS)
    if written != len(command):
        raise ProbeError("short bulk write: %d of %d bytes" % (written, len(command)))
    reply = bytes(dev.read(EP_IN, len(command), TIMEOUT_MS))
    if len(reply) != len(command):
        raise ProbeError("short bulk read: %d of %d bytes" % (len(reply), len(command)))
    if doorbell:
        dev.ctrl_transfer(0xC0, 0xDA, 0x0007, 0x0000, 2, TIMEOUT_MS)
    return reply


def open_session(dev):
    """Open the session with the two mandatory 0xdb control writes.

    The 0x000b re-init pair of the Windows driver stalls on this chip, so
    only the 0x0006 pair (wIndex 0 and 1) is sent.
    """
    for index in (0, 1):
        dev.ctrl_transfer(0x40, 0xDB, 0x0006, index, b"", TIMEOUT_MS)


def setup_profile(dev, profile):
    """Reset the sensor and apply the register settings of a profile."""
    exchange(dev, "f8", doorbell=True)
    time.sleep(0.05)
    exchange(dev, "1c00")
    exchange(dev, "1c00")
    if profile["init"]:
        for command in INIT_COMMANDS:
            exchange(dev, command)
    for command in profile["extra"]:
        exchange(dev, command)


def probe_round(dev, delay):
    """Run one detection round and return ``(irq, zones, error_status)``."""
    exchange(dev, "1c00")                      # clear the interrupt status
    exchange(dev, "20", doorbell=True)         # arm the sensor
    time.sleep(delay)
    status = exchange(dev, "1c00")             # interrupt register and state
    zone_reply = exchange(dev, "d40000")       # capacitive zone mask
    error_status = exchange(dev, "3800")       # read error status with clear
    irq = status[1] if len(status) > 1 else 0x00
    zones = int.from_bytes(zone_reply[1:3], "big") & 0x0FFF if len(zone_reply) >= 3 else 0
    return irq, zones, error_status


def describe(irq, zones, error_status):
    """Describe one detection round.

    Only bit 0x01 of the interrupt register is a finger indication; the
    command-done (0x80) and FIFO (0x20) bits say nothing about a finger.
    The bit is not reported at all when the register is in the error or the
    reboot state, because it is meaningless there.
    """
    failed = bool(irq & IRQ_ERROR) or irq == IRQ_REBOOT
    finger = 0 if failed or not irq & IRQ_FINGER_DOWN else 1
    return ("IRQ=0x%02x bits=%d finger_down=%d status=%s zones=0x%03x (%d zone(s)) "
            "error_status=0x%s"
            % (irq, bin(irq).count("1"), finger,
               "ERROR" if failed else "ok", zones, bin(zones).count("1"),
               error_status.hex()))


def run_profile(dev, name, count, delay):
    """Run the detection rounds of one profile."""
    profile = PROFILES[name]
    print("=== profile %s ===" % name)
    setup_profile(dev, profile)
    print("  init: %s, overrides: %s"
          % ("yes" if profile["init"] else "no",
             ", ".join(profile["extra"]) or "none"))
    for round_number in range(1, count + 1):
        irq, zones, error_status = probe_round(dev, delay)
        print("  round %d: %s" % (round_number, describe(irq, zones, error_status)))
        sys.stdout.flush()


def positive_int(value):
    """argparse type: an integer greater than zero."""
    try:
        number = int(value)
    except ValueError:
        raise argparse.ArgumentTypeError("%r is not an integer" % value)
    if number <= 0:
        raise argparse.ArgumentTypeError("%r must be > 0" % value)
    return number


def positive_float(value):
    """argparse type: a finite number greater than zero."""
    try:
        number = float(value)
    except ValueError:
        raise argparse.ArgumentTypeError("%r is not a number" % value)
    if not number > 0 or number == float("inf"):
        raise argparse.ArgumentTypeError("%r must be a finite number > 0" % value)
    return number


def build_parser():
    parser = argparse.ArgumentParser(
        prog="probe-finger-profiles.py",
        description=("Compare the finger detection register profiles of the Samsung "
                     "04e8:7301 sensor.  No image is read, nothing is written to disk "
                     "and no fingerprint is enrolled."),
        epilog=("Requirements: USB access to the reader and a stopped fprintd (sudo "
                "systemctl stop fprintd before, sudo systemctl start fprintd after)."),
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--count", type=positive_int, default=3,
                        help="detection rounds per profile (default: %(default)s)")
    parser.add_argument("--delay", type=positive_float, default=0.05,
                        help="seconds between arming the sensor and reading the "
                             "registers (default: %(default)s)")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    dev = usb.core.find(idVendor=VENDOR_ID, idProduct=PRODUCT_ID)
    if dev is None:
        print("error: no 04e8:7301 sensor found", file=sys.stderr)
        return 1
    try:
        try:
            dev.set_configuration()
        except usb.core.USBError as exc:
            print("warning: set_configuration failed: %s" % exc, file=sys.stderr)
        usb.util.claim_interface(dev, INTERFACE)
        open_session(dev)
        print("probing 04e8:7301, %d round(s) per profile, delay %.3fs"
              % (args.count, args.delay))
        for name in PROFILE_ORDER:
            run_profile(dev, name, args.count, args.delay)
        print("done")
        return 0
    except (usb.core.USBError, ProbeError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130
    finally:
        try:
            usb.util.release_interface(dev, INTERFACE)
        except (usb.core.USBError, NotImplementedError) as exc:
            print("warning: release_interface failed: %s" % exc, file=sys.stderr)
        finally:
            usb.util.dispose_resources(dev)


if __name__ == "__main__":
    sys.exit(main())
