#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Manually switch the power-button LED through the ELAN 04f3:0c4b sensor.

Use this to check that your machine reacts to the LED commands before
installing the shim. fprintd must not hold the sensor while it runs:

  sudo systemctl stop fprintd
  sudo python3 tools/led-test.py            # interactive: green, then restore
  sudo python3 tools/led-test.py restore    # only restore the LED
  sudo systemctl start fprintd

Requires pyusb (Fedora: python3-pyusb, Debian/Ubuntu: python3-usb,
Arch: python-pyusb). Sends only the bytes the Windows driver sends.
"""
import sys
import usb.core
import usb.util

VID, PID = 0x04F3, 0x0C4B
EP_OUT = 0x01

GREEN = ["460790010400000000", "460780010440000000"]
RESTORE = ["460780010440000000", "460790010410000000"]


def send(dev, cmds):
    for c in cmds:
        dev.write(EP_OUT, bytes.fromhex(c), timeout=1000)
        print(f"  > {c}")


def main():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        sys.exit("ELAN 04f3:0c4b not found (check lsusb)")
    try:
        usb.util.claim_interface(dev, 0)
    except usb.core.USBError as e:
        sys.exit(f"cannot claim the sensor ({e}). Is fprintd still running? "
                 "Run: sudo systemctl stop fprintd")
    try:
        if len(sys.argv) > 1 and sys.argv[1] == "restore":
            send(dev, RESTORE)
            return
        input("Press Enter to turn the LED green...")
        send(dev, GREEN)
        input("The LED should flash and then stay green. Press Enter to restore...")
        send(dev, RESTORE)
        print("The LED should be back to its normal colour.")
    finally:
        usb.util.release_interface(dev, 0)
        usb.util.dispose_resources(dev)
        print("Now run: sudo systemctl start fprintd")


if __name__ == "__main__":
    main()
