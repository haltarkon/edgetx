# Channel stream protocol, version 1

With the USB-VCP serial port in **Channels** mode, the radio sends all 32 channel outputs as fixed-size binary frames, one after every mixer run. This page is the reference for applications that read the stream; [README.md](README.md) explains how to set up the radio and has a complete reader in Python. The firmware side is `radio/src/channel_stream.h`.

## Frame format

All multi-byte fields are little-endian. The fields are not padded, so read them byte by byte, or overlay a packed structure.

| Offset | Size | Field | Meaning |
| ---: | ---: | --- | --- |
| 0 | 1 | `sync` | `0xEC` |
| 1 | 1 | `version` | Protocol version: `1` |
| 2 | 1 | `len` | Number of bytes after this one: type, payload and CRC. 76 for type 1. |
| 3 | 1 | `type` | `0x01`: channels |
| 4 | 1 | `seq` | Mixer run counter, wraps from 255 to 0 |
| 5 | 4 | `time_us` | Radio microsecond clock at the start of the mixer run |
| 9 | 2 | `period_us` | Mixer period in microseconds |
| 11 | 2 | `flags` | Reserved, 0 in version 1 |
| 13 | 64 | `ch[32]` | Channel outputs, `int16` each |
| 77 | 2 | `crc` | CRC-16/CCITT-FALSE over bytes 1 to 76 |

A frame is 79 bytes long. Byte map, 16 bytes per row (`E` envelope, `H` header, `C` channels, `R` CRC):

```text
offset  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
     0  E  E  E  H  H  H  H  H  H  H  H  H  H  C  C  C     sync ver len type seq time_us period flags CH1..
    16  C  C  C  C  C  C  C  C  C  C  C  C  C  C  C  C
    32  C  C  C  C  C  C  C  C  C  C  C  C  C  C  C  C
    48  C  C  C  C  C  C  C  C  C  C  C  C  C  C  C  C
    64  C  C  C  C  C  C  C  C  C  C  C  C  C  R  R        ..CH32 crc
```

Example frame, with values from a capture (`seq` 248, `time_us` 73556818, `period_us` 1000, CH1–CH4 = 683, 621, 8, −691, CH5–CH10 = −1024, CH11 = 1, CH12 = 5, the rest 0):

```text
EC 01 4C 01 F8 52 63 62 04 E8 03 00 00 AB 02 6D
02 08 00 4D FD 00 FC 00 FC 00 FC 00 FC 00 FC 00
FC 01 00 05 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 5B 86
```

## Fields

- **`version`** is the protocol version, 1 for the format on this page. It tells an application whether it can read the frame at all; see the compatibility rules below.
- **`seq`** counts mixer runs, not frames. A gap means the radio skipped those runs: it does not send while the host has not yet taken the previous frame, so the host always gets the newest sample instead of a queue of old ones. Gaps appear only when the host stops reading.
- **`time_us`** is taken just before the mixer reads the sticks. It wraps every 2³² µs (71.6 minutes), so use differences. It shows how far apart two samples were taken, and with the host's arrival times it measures the delay that USB and the host added.
- **`period_us`** is 1000 while the radio runs its 1 kHz schedule, which it does while the stream is active and no RF module sets the period. Otherwise the module sets it.
- **`ch`** holds the channel outputs as Lua's `getOutputValue()` returns them: −1024 to +1024 is −100 % to +100 %, and extended limits reach ±1536 (±150 %). The pulse width in microseconds is 1500 + value / 2.
- **`crc`** is CRC-16/CCITT-FALSE: polynomial `0x1021`, initial value `0xFFFF`, no reflection, no final XOR. Its check value for the ASCII string `123456789` is `0x29B1`. In Python it is `binascii.crc_hqx(data, 0xFFFF)`.

## Compatibility rules

- Every version keeps `sync`, `version` and `len` at offsets 0 to 2, and the same CRC over everything between `sync` and `crc`. An application can therefore check any frame, and skip one whose version it does not know. When it sees such frames, the radio's firmware is newer than the application: tell the user.
- The version changes only when an application written for the previous version would misread a frame. Appending fields does not change it: a known type may grow, with `len` growing to match. Decode the fields you know, and ignore the rest of the payload.
- Skip frames of an unknown `type` by their `len`.
- Ignore `flags` bits you do not know.

## Parsing a byte stream

A serial port delivers bytes, not frames, so find the frame boundaries yourself:

```text
loop:
  find the next 0xEC in the buffer; drop the bytes before it
  wait until the version and len bytes are there
  if len < 3 or len > 250: drop one byte, loop
  wait until all 3 + len bytes are there
  if the CRC over bytes 1 .. len does not match the last two bytes:
      drop one byte, loop          (a false sync inside data, or corruption)
  if version is not 1: report it, drop the frame's 3 + len bytes, loop
  handle the frame by type; drop its 3 + len bytes
```

This finds the stream again after opening the port in the middle of a frame and after corrupted data. The Python reader in [README.md](README.md) implements it.

## Measuring the link

Three numbers describe a capture well:

- **skipped mixer runs**: the `seq` gaps described above;
- **radio interval**: the `time_us` difference between consecutive frames, which shows the mixer's own regularity;
- **arrival delay above the fastest frame**: the host's arrival time minus `time_us`, with the drift between the two clocks removed. The earliest arrivals mark the fastest path from mixer to application; the distribution above it is the delay that USB, the host's serial driver and the reading thread added.
