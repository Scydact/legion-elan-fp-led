# ELAN 04f3:0c4b LED protocol notes

Reverse engineered from a USBPcap capture of Windows Hello on a Legion 7 16IAX7
(Windows 11 25H2) and confirmed by replaying the bytes from Linux with
[`tools/led-test.py`](../tools/led-test.py).

## Transport

The sensor exposes one vendor-specific interface with bulk endpoints only:

| Endpoint | Direction | Use |
|---|---|---|
| 0x01 | OUT | commands |
| 0x82 | IN  | image data (12800-byte frames) |
| 0x83 | IN  | short command responses / finger interrupt |
| 0x81, 0x03 | IN / OUT | not seen in use |

There is no HID or LED interface. The LED is driven by the sensor itself; the
ACPI tables (DSDT/SSDTs) and Lenovo WMI methods have no power-button LED or
fingerprint control, and the flash still happens on Windows with every Lenovo
service stopped.

## LED commands

The LED commands look like register writes:

```
46 07 <reg> 01 04 <val> 00 00 00
```

| Purpose | Bytes |
|---|---|
| Green on (1/2) | `46 07 90 01 04 00 00 00 00` (reg 0x90 = 0x00) |
| Green on (2/2) | `46 07 80 01 04 40 00 00 00` (reg 0x80 = 0x40) |
| Restore (1/2)  | `46 07 80 01 04 40 00 00 00` (reg 0x80 = 0x40) |
| Restore (2/2)  | `46 07 90 01 04 10 00 00 00` (reg 0x90 = 0x10) |

After "green on" the LED flashes green a few times and then stays green. After
"restore" it returns to the colour the EC shows for the current power profile
(white, red, blue...). Neither write gets a reply.

Which of the two writes is strictly required, and whether other values give
other colours or modes, has not been tested yet.

## Windows Hello sequence

```
9a10            -> 409a1000   status query
4031
9a10            -> 409a1000
460790010400000000            LED green (1/2)
460780010440000000            LED green (2/2)
403f                          wait for finger
                <- 55 (ep 0x83)  finger touched
0009 / 12800-byte read on 0x82   repeated, image capture
460780010440000000            restore (1/2)
9a10            -> 409a1000
000b                          stop
460790010410000000            restore (2/2)
```

## Linux (libfprint TOD blob) sequence

The Lenovo/ELAN TOD driver never sends the LED writes. During a verify it
sends, all asynchronously through `libusb_submit_transfer`:

```
4019, 40210a, 000c, 0009, 402106, 402107, 4024   setup
403f                                             wait for finger
0009, 403e, 0009                                 capture / finger-off checks
000b                                             stop
```

The shim hooks on `403f` (inject "green on" just before it) and `000b` (inject
"restore" just after it), plus `libusb_release_interface` / `libusb_close` as
a safety net.
