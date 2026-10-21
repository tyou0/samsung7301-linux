#!/usr/bin/env python3
"""Exercise the installed Samsung driver repeatedly (requires USB access).

Run with fprintd stopped. Images remain in memory and are not saved. This
checks driver lifetime handling, not image quality or fingerprint matching.
"""
import threading

import gi

gi.require_version("FPrint", "2.0")
from gi.repository import FPrint, Gio, GLib

context = FPrint.Context()
context.enumerate()
device = next(d for d in context.get_devices() if d.get_driver() == "samsung7301")
for cycle in range(3):
    device.open_sync(None)
    try:
        for capture in range(2):
            cancel = Gio.Cancellable()
            timer = threading.Timer(0.4, cancel.cancel)
            timer.start()
            try:
                device.capture_sync(True, cancel)
                result = "captured"
            except GLib.Error as error:
                if not error.matches(Gio.io_error_quark(), Gio.IOErrorEnum.CANCELLED):
                    raise
                result = "cancelled"
            finally:
                timer.cancel()
                timer.join()
            print(f"Cycle {cycle + 1}, capture {capture + 1}: {result}")
    finally:
        device.close_sync(None)
print("PASS: repeated open/capture/close; matching remains unverified")
