#!/usr/bin/env python3
"""Replay the ELAN 04f3:0c4b LED commands seen in a Windows Hello USB capture.

Run as root with fprintd stopped:
  sudo systemctl stop fprintd
  sudo python3 led-test.py
  sudo systemctl start fprintd

Only sends the exact bytes the Windows driver sent. Each step waits for Enter
so you can watch the power button LED.
"""
import sys, time
import usb.core, usb.util

VID, PID = 0x04F3, 0x0C4B
EP_OUT, EP_RESP = 0x01, 0x83

dev = usb.core.find(idVendor=VID, idProduct=PID)
if dev is None:
    sys.exit("ELAN 04f3:0c4b not found")
if dev.is_kernel_driver_active(0):
    sys.exit("interface 0 is held by a kernel driver, aborting")
usb.util.claim_interface(dev, 0)

def send(hexstr, read=0):
    dev.write(EP_OUT, bytes.fromhex(hexstr), timeout=1000)
    out = ""
    if read:
        try:
            out = bytes(dev.read(EP_RESP, read, timeout=1000)).hex()
        except usb.core.USBTimeoutError:
            out = "(no reply)"
    print(f"  > {hexstr}  {out}")

def step(title):
    input(f"\n{title}\nPress Enter to send...")

try:
    step("Step 1: status query + 40 31 (Windows sends this first)")
    send("9a10", read=4)
    send("4031")
    send("9a10", read=4)
    input("Did the LED change? Note it, then press Enter.")

    step("Step 2: 46 07 90 ... 00 and 46 07 80 ... 40 (sent right before waiting for a finger)")
    send("460790010400000000")
    send("460780010440000000")
    input("Did the LED start flashing green? Note it, then press Enter.")

    step("Step 3: restore sequence Windows sent after the scan")
    send("460780010440000000")
    send("9a10", read=4)
    send("000b")
    send("460790010410000000")
    input("Is the LED back to normal? Press Enter to finish.")
finally:
    usb.util.release_interface(dev, 0)
    usb.util.dispose_resources(dev)
    print("\nDone. Now run: sudo systemctl start fprintd")
