#!/usr/bin/env python3
"""Diagnose the installed libfprint stack for the Samsung 04e8:7301 reader.

Read only: nothing is enrolled, verified or deleted and nothing is written
to disk - captured frames stay in RAM.  libfprint 1.94 has no image writer
in its GI typelib, so there is no --save-dir.

Requirements: USB access to the reader (contrib/99-samsung-7301.rules) and a
stopped fprintd - libfprint opens the reader exclusively, so every open
fails with "device is busy" while fprintd holds it:

    sudo systemctl stop fprintd      # before testing
    sudo systemctl start fprintd     # after testing

Modes: inspect (driver, scan type, enrolment stages, capabilities), sample
(capture N frames, default 3, plus image statistics), cancel (repeat
open/capture/cancel/close).  A cancelled capture is not a capture.

The cancel mode can cancel an enrolment instead of a capture
(--operation enroll).  That talks to the driver through libfprint
directly, so fprintd is not involved at all: the print stays in RAM and
is discarded, nothing is enrolled in or stored by the system.  It
exercises an FpDevice driver without an image capture.

Exit status: 0 ok, 1 failed, 2 incomplete.
"""
import argparse
import hashlib
import math
import statistics
import sys
import time
from itertools import combinations

DEFAULT_DRIVER = "samsung7301"
OP_TIMEOUT = 10.0


class DiagnosticError(Exception):
    """Fatal error, already formatted for the terminal."""


def positive(value, integral=False):
    """argparse type: a finite number greater than zero."""
    try:
        number = int(value) if integral else float(value)
        if not math.isfinite(number) or number <= 0:
            raise ValueError
    except ValueError:
        raise argparse.ArgumentTypeError("%r must be a finite number > 0" % value)
    return number


def select_device(FPrint, driver=None):
    """Return ``(context, device)``; the context has to outlive the device.

    It owns the device objects and finalises them - and the libusb handle
    with them - when it is collected.
    """
    context = FPrint.Context()
    context.enumerate()
    devices = list(context.get_devices())
    if not devices:
        raise DiagnosticError("no fingerprint readers found; check the cable, the udev "
                              "rule in contrib/99-samsung-7301.rules and your USB access")
    selected = [d for d in devices if not driver or d.get_driver() == driver]
    if not selected:
        raise DiagnosticError("no device matched driver=%r; available: %s"
                              % (driver, ", ".join(d.get_driver() for d in devices)))
    if len(selected) > 1:
        raise DiagnosticError("%d devices match, only one reader is supported"
                              % len(selected))
    return context, selected[0]


def call(Gio, timeout, func, *args):
    """Run a libfprint call with a finite, cancellable timeout.

    Returns ``(result, status, elapsed)``; the status is ``ok``,
    ``cancelled`` or ``error: ...``.
    """
    from gi.repository import GLib  # GLib is not visible in this module scope

    cancellable = Gio.Cancellable()

    # The timeout is handled by a source in the main context that the
    # synchronous libfprint call iterates, so the cancellation is delivered
    # on that thread.  Cancelling from a second thread races with the
    # cancellation handling of libfprint itself, which unrefs its idle
    # cancel source while the main loop may already have cleared it.
    context = GLib.MainContext.get_thread_default()
    if context is None:
        context = GLib.MainContext.default()

    def on_timeout():
        cancellable.cancel()
        return GLib.SOURCE_REMOVE

    source = GLib.timeout_source_new(max(1, int(timeout * 1000)))
    source.set_callback(on_timeout)
    source.attach(context)

    started = time.monotonic()
    try:
        return func(*args, cancellable=cancellable), "ok", time.monotonic() - started
    except GLib.Error as error:
        try:
            cancelled = error.matches(Gio.io_error_quark(), Gio.IOErrorEnum.CANCELLED)
        except AttributeError:
            cancelled = False
        status = "cancelled" if cancelled else "error: %s:%s %s" % (
            error.domain, error.code, error.message)
        return None, status, time.monotonic() - started
    finally:
        # The source removes itself when it fires, so destroying it here is
        # safe in both cases and keeps it from touching the cancellable
        # after the call is over.
        source.destroy()


def frame_stats(image):
    """Statistics of one frame; the pixels stay in memory only."""
    data = bytes(image.get_data() or b"")
    stats = {"width": int(image.get_width()), "height": int(image.get_height()),
             "ppmm": float(image.get_ppmm()), "length": len(data), "data": data}
    if not data:
        return dict(stats, min=None, max=None, mean=None, stddev=None, sha256=None)
    return dict(stats, min=min(data), max=max(data), mean=statistics.fmean(data),
                stddev=statistics.pstdev(data), sha256=hashlib.sha256(data).hexdigest())


def compare_frames(first, second):
    """Normalized correlation and mean absolute difference of two frames."""
    left, right = first["data"], second["data"]
    if not left or len(left) != len(right):
        return None
    try:
        correlation = statistics.correlation(left, right)
    except statistics.StatisticsError:
        correlation = None
    return {"correlation": correlation,
            "mad": sum(abs(x - y) for x, y in zip(left, right)) / len(left)}


def mode_inspect(device, FPrint, Gio, GLib, args):
    print("=== libfprint diagnostic (inspect) ===")
    features = device.get_features()
    names = [name for name, value in FPrint.DeviceFeature.__members__.items()
             if value != FPrint.DeviceFeature.NONE and features & value]
    for label, value in (("driver", device.get_driver()), ("name", device.get_name()),
                         ("device id", device.get_device_id()),
                         ("scan type", device.get_scan_type().value_nick),
                         ("enroll stages", device.get_nr_enroll_stages()),
                         ("features", ", ".join(names) or "(none)")):
        print("%-22s %s" % (label + ":", value))
    return 0


def mode_capture(device, FPrint, Gio, GLib, args):
    """Capture frames (sample) or cancelled captures (cancel mode).

    Every attempt opens the reader, starts one capture and closes the reader
    again.  A cancelled capture is not a capture.

    In cancel mode the operation may also be an enrolment: libfprint is
    called directly, so fprintd never sees the print and nothing is
    enrolled or stored.  The returned print is discarded.
    """
    cancelling = args.mode == "cancel"
    operation = args.operation
    enrolling = cancelling and operation == "enroll"
    count = args.cycles if cancelling else args.count
    timeout = args.cancel_after if cancelling else args.timeout
    label = "cycle" if cancelling else "frame"
    print("=== libfprint diagnostic (%s) ===" % args.mode)
    print("device: %s (%s), %d %s(s), %.1fs timeout each"
          % (device.get_driver(), device.get_name(), count, label, timeout))
    if enrolling:
        print("operation: enroll - libfprint is called directly, fprintd is not "
              "involved, so nothing is enrolled in or stored by the system")
    frames, captured, cancelled, failed, completed = [], 0, 0, 0, 0
    for index in range(1, count + 1):
        opened = False
        try:
            _, status, _ = call(Gio, OP_TIMEOUT, device.open_sync)
            if status != "ok":
                raise DiagnosticError("could not open the device: %s (if fprintd runs it "
                                      "holds the reader: sudo systemctl stop fprintd)"
                                      % status)
            opened = True
            if enrolling:
                enrolled, status, elapsed = call(Gio, timeout, device.enroll_sync,
                                                  FPrint.Print.new(device))
                # The print is thrown away: it only lives in RAM and never
                # reaches fprintd, so nothing is stored by the system.
                del enrolled
            else:
                image, status, elapsed = call(Gio, timeout, device.capture_sync,
                                              args.wait_for_finger)
            if status == "cancelled":
                cancelled += 1
                print("%s %d: CANCELLED after %.2fs (not a capture)"
                      % (label, index, elapsed))
            elif status != "ok":
                failed += 1
                print("%s %d: FAILED after %.2fs: %s" % (label, index, elapsed, status))
            elif cancelling:
                completed += 1
                print("%s %d: %s completed in %.2fs before the timer fired, the "
                      "cancellation was not exercised" % (label, index, operation, elapsed))
            else:
                captured += 1
                stats = frame_stats(image)
                stats["index"] = index
                frames.append(stats)
                print("%s %d: captured in %.2fs" % (label, index, elapsed))
                print("  size:        %dx%d, ppmm=%.3f, %d pixel byte(s)\n"
                      "  levels:      min=%s max=%s mean=%.2f stddev=%.2f\n"
                      "  sha256:      %s"
                      % (stats["width"], stats["height"], stats["ppmm"], stats["length"],
                         stats["min"], stats["max"], stats["mean"], stats["stddev"],
                         stats["sha256"]))
        except DiagnosticError as exc:
            failed += 1
            print("%s %d: FAILED: %s" % (label, index, exc))
        finally:
            if opened:
                _, status, _ = call(Gio, OP_TIMEOUT, device.close_sync)
                if status != "ok":
                    failed += 1
                    print("warning: close failed: %s" % status, file=sys.stderr)
    for first, second in combinations(frames, 2):
        result = compare_frames(first, second)
        correlation = result["correlation"] if result else None
        print("frame %d vs frame %d: correlation=%s mean-abs-diff=%s"
              % (first["index"], second["index"],
                 "%.6f" % correlation if correlation is not None else "n/a",
                 "%.4f" % result["mad"] if result else "n/a"))
    print("summary: %d captured, %d cancelled, %d failed" % (captured, cancelled, failed))
    if failed:
        print("result:  FAILED - at least one capture did not succeed")
        return 1
    if cancelling and completed:
        print("result:  INCOMPLETE - use a shorter --cancel-after so the timer fires "
              "during the transfer")
        return 2
    if not cancelling and captured < count:
        print("result:  INCOMPLETE - not every requested frame was captured")
        return 2
    print("result:  OK - the reader passed the test")
    return 0


def build_parser():
    parser = argparse.ArgumentParser(
        prog="diagnose-libfprint.py",
        description=("Diagnose the installed libfprint stack for the Samsung 04e8:7301 "
                     "reader.  Read only: nothing is written to disk, no fingerprint "
                     "is enrolled, verified or deleted and none is stored.  The cancel "
                     "mode can cancel an enrolment as well (--operation enroll): the "
                     "driver scans a finger, but libfprint is called directly, so "
                     "fprintd is not involved and the print is discarded in RAM."),
        epilog=("Requirements: USB access to the reader and a stopped fprintd (sudo "
                "systemctl stop fprintd before, sudo systemctl start fprintd after).  "
                "Exit status: 0 ok, 1 failed, 2 incomplete."),
        formatter_class=argparse.RawDescriptionHelpFormatter)
    subs = parser.add_subparsers(dest="mode", metavar="mode", required=True)
    modes = (("inspect", mode_inspect, "driver, scan type, stages, capabilities"),
             ("sample", mode_capture, "capture frames and report image statistics"),
             ("cancel", mode_capture, "repeat open/capture/cancel/close"))
    extras = {"sample": (("--count", lambda v: positive(v, True), 3, "frames to capture"),
                         ("--timeout", positive, 3.0, "per frame timeout in seconds")),
              "cancel": (("--cycles", lambda v: positive(v, True), 3, "lifecycle cycles"),
                         ("--cancel-after", positive, 0.4, "delay before cancelling"))}
    for name, func, summary in modes:
        sub = subs.add_parser(name, help=summary)
        sub.add_argument("--driver", default=DEFAULT_DRIVER,
                         help="driver name (default: %(default)s, '' for any)")
        for flag, kind, default, text in extras.get(name, ()):
            sub.add_argument(flag, type=kind, default=default,
                             help="%s (default: %%(default)s)" % text)
        if name != "inspect":
            sub.add_argument("--no-wait", dest="wait_for_finger", action="store_false",
                             help="do not wait for a finger before the transfer")
        if name == "cancel":
            sub.add_argument("--operation", choices=("capture", "enroll"),
                             default="capture",
                             help="operation to cancel: capture (default: %(default)s) or "
                                  "enroll (libfprint is called directly, fprintd is not "
                                  "involved and the print is discarded, so nothing is "
                                  "enrolled in or stored by the system)")
        sub.set_defaults(func=func, wait_for_finger=True, operation="capture")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        import gi
        gi.require_version("FPrint", "2.0")
        from gi.repository import FPrint, Gio, GLib
    except (ImportError, ValueError) as exc:
        print("error: libfprint-2 introspection data is missing (%s)" % exc, file=sys.stderr)
        return 1
    try:
        context, device = select_device(FPrint, args.driver)
        try:
            return args.func(device, FPrint, Gio, GLib, args)
        finally:
            # The context owns the device and its libusb handle.  Drop the
            # device first and only then the context, and only after the
            # reader has been closed again, so that libusb does not exit
            # while a device is still referenced.
            del device
            del context
    except DiagnosticError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
