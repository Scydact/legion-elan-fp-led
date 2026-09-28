# ELAN 04f3:0c4b LED protocol notes

Reverse engineered from USBPcap captures of Windows Hello on a Legion 7 16IAX7
(Windows 11 25H2), then confirmed and explored by replaying bytes from Linux
with [`tools/led-test.py`](../tools/led-test.py) and a register probe. No
public datasheet or protocol documentation for this sensor is known.

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
46 07 <reg> 01 04 <b5> <b6> <b7> 00
```

Windows always sends them in these combinations:

| Purpose | Bytes |
|---|---|
| Green on (1/2) | `46 07 90 01 04 00 00 00 00` (reg 0x90 = 00) |
| Green on (2/2) | `46 07 80 01 04 40 00 00 00` (reg 0x80 = 40) |
| Failed match   | `46 07 90 01 04 20 c0 30 00` (reg 0x90 = 20 c0 30) |
| Re-arm after a failed match (1/2) | `46 07 90 01 04 40 00 00 00` (reg 0x90 = 40) |
| Re-arm after a failed match (2/2) | `46 07 80 01 04 40 00 00 00` (reg 0x80 = 40) |
| Restore (1/2)  | `46 07 80 01 04 40 00 00 00` (reg 0x80 = 40) |
| Restore (2/2)  | `46 07 90 01 04 10 00 00 00` (reg 0x90 = 10) |

None of the writes gets a reply. After "restore" the LED returns to the colour
the EC shows for the current power profile (white, red, blue...).

### Register probe results

Each value below was sent from Linux with fprintd stopped, followed by the
restore sequence, and watched with the LED in the blue (quiet) profile colour
so the sensor's white can't be confused with the EC's.

| Writes | LED |
|---|---|
| 0x90 = `00 00 00` | green flash, then solid green |
| 0x90 = `40 00 00` | white flash, then solid green |
| 0x90 = `20 c0 30` / `20 c0 00` / `20 c0 60` | white flash, then solid green |
| 0x90 = `20 80 30` | green flash, then solid green |
| 0x90 = `20 00 00` / `20 00 30` / `20 ff ff` / `30 00 00` | no change (profile colour) |
| 0x90 = `10 00 00` | back to the EC (restore) |
| 0x80 = `40` alone | no change |
| 0x90 = `00`, then 0x80 = `00` / `10` / `20` / `80` / `c0` | same as 0x80 = `40` |

Reading of the results (a hypothesis, not documentation):

- Register **0x90** controls the LED; 0x80 has no visible effect.
- `b5` is a mode: `00` preset green flash, `40` preset white flash, `20`
  custom pattern taken from `b6`, `10` hand the LED back to the EC.
- In custom mode `b6` bit 7 enables the flash and bit 6 picks white over
  green (`c0` white, `80` green, `00` none); `ff` is rejected.
- `b7` (`30`, `60`, `00`) made no visible difference; it may be a duration or
  count.
- Every flash ends in solid green, so solid green looks like the sensor's
  fixed "armed" state. No value produced any colour other than green or
  white.

## Windows Hello sequence

A successful match:

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

A failed match followed by a retry (the sequence repeats per attempt):

```
<- 55 (ep 0x83)                  finger touched
0009 / image reads               capture
460790010420c03000            failed match: white flash
9a10            -> 409a1000
4031
9a10            -> 409a1000
460790010440000000            re-arm (1/2): white flash
460780010440000000            re-arm (2/2)
403f                          wait for finger again
```

Before the attempt that finally matched, Windows restored the LED
(0x90 = 10) and armed it with 0x90 = `20 00 00` + 0x80 = `40`, which by itself
shows no change.

## Linux (libfprint TOD blob) sequence

The Lenovo/ELAN TOD driver never sends the LED writes. During a verify it
sends, all asynchronously through `libusb_submit_transfer`:

```
4019, 40210a, 000c, 0009, 402106, 402107, 4024   setup
403f                                             wait for finger
0009, 403e, 0009                                 capture / finger-off checks
000b                                             stop
```

The match is computed on the host after the capture, so this sequence is
identical for a wrong and a right finger. The blob reports the result through
libfprint just before `000b`:

- `fpi_device_verify_report(device, FpiMatchResult result, FpPrint *print, GError *error)`
  with `FPI_MATCH_ERROR = -1`, `FPI_MATCH_FAIL = 0`, `FPI_MATCH_SUCCESS = 1`
  (values read from the installed `libfprint-2-tod.so.1` GType);
- `fpi_device_identify_report(device, FpPrint *match, FpPrint *print, GError *error)`
  with `match == NULL` for no match.

Both are imported by the blob as `@LIBFPRINT_TOD_1.0.0` and exported by
`libfprint-2-tod.so.1`.

What happens next depends on the client:

- `fprintd-verify` (one attempt): `verify result=0`, `000b`, then
  `release_interface` and `close` right away.
- KDE lock screen / PAM (identify with retries): `identify match=no`, `000b`,
  then straight into the next attempt (`0009, 402106, 402107, 4024, 403f`)
  without closing the device; `release_interface` / `close` only after the
  final match.

The shim hooks on `403f` (inject "green on" just before it) and `000b` (inject
"restore" just after it), plus `libusb_release_interface` / `libusb_close` as
a safety net, and on the two report functions for the fail flash (see the
README's "Failed matches" section).

## Notes for contributors

- libusb's own synchronous `libusb_bulk_transfer` calls
  `libusb_submit_transfer` internally, and that call resolves to the shim's
  wrapper too. Commands the shim sends itself are marked with a thread-local
  flag so the wrappers pass them straight through; without it, the
  synchronous restore at release re-entered the shim while it held its mutex
  and hung fprintd.
- The fail-flash restore timer runs in a detached thread and submits its
  transfer asynchronously; libusb allows submitting from any thread, and
  fprintd's own event loop completes it.
