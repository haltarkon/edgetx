# USB-VCP channel stream

This fork adds a serial port mode, **Channels**, for the radio's USB serial port (USB-VCP). In this mode the radio sends all 32 channel outputs as one fixed 79-byte binary frame after every mixer run: up to 1000 frames per second, with no Lua script, while the radio stays fully usable. The radio shows up as an ordinary USB serial port, so Windows 10/11, macOS and Linux need no driver.

- [protocol.md](protocol.md) specifies the frames, for applications that read the stream.
- [speed-improvements.md](speed-improvements.md) lists every change that makes the data arrive sooner, with measurements.

## Branches

Each branch carries the same three commits on top of its base, plus these notes.

| Branch | Base | Status |
| --- | --- | --- |
| `channel-stream` | EdgeTX `main` at `e066572340` (2026-09-15, before 3.0) | Flashed and measured on a Radiomaster GX12; builds for TX16S, TX12 MK2 and TX12 |
| `channel-stream-v2.12.4` | release v2.12.4 | Builds for TX16S, TX12 MK2 and TX12; EdgeTX unit tests pass; not yet flashed |
| `channel-stream-v2.11.7` | release v2.11.7 | Same as v2.12.4 |
| `channel-stream-v2.10.7` | release v2.10.7 | Same as v2.12.4 |

The three commits:

1. `fix(serial): ignore serial modes of ports the radio does not have`
2. `feat(usb): send USB-VCP data at once instead of every 16th frame`
3. `feat: channel stream serial mode for USB-VCP`

The first two also help without the new mode: the first fixes a settings bug described at the end of this page, and the second makes everything sent over the USB serial port leave the radio sooner, including what Lua scripts write with `serialWrite()`.

## On the radio

1. Flash firmware built from one of the branches (see [Building](#building)). Back up the SD card's `RADIO` and `MODELS` folders first.
2. Open **SYS → Hardware** and set the **USB-VCP** serial port to **Channels**. The mode is offered on USB-VCP only. In `RADIO/radio.yml` it is stored as:

   ```yaml
   serialPort:
     VCP:
       mode: CHANNELS
   ```

3. Connect the USB cable and choose **USB Serial (VCP)**. To skip that question, set **USB mode** to **Serial** in the radio setup.
4. For 1000 frames per second, turn the model's internal and external RF modules off. While a module is active, the module sets the mixer period, and the radio sends one frame per module frame, for example 250 or 500 per second. The frames say which: see `period_us` in [protocol.md](protocol.md).

Menus, model settings, trims and Lua tools keep working while the radio streams. Stock EdgeTX does not know the Channels mode, so after flashing stock firmware again, check the USB-VCP setting.

## On the computer

The radio appears as a standard USB CDC-ACM serial port named "*radio name* Serial Port", with USB VID `0483` and PID `5740`. On Linux the user must be in the `dialout` group. The baud rate and the other line settings are ignored. At 1000 frames per second the stream is 79 kB/s. The radio only sends: bytes written by the host are ignored.

A serial port delivers bytes, not frames, so the reader must find the frames itself. This reader does that, recovers from a port opened in the middle of a frame or from corrupted bytes, and skips frames of a protocol version or type it does not know. It needs only the Python standard library:

```python
import binascii
import struct

SYNC = 0xEC


def frames(chunks):
    """Yield (seq, time_us, period_us, flags, channels) for each valid
    version 1 channels frame found in an iterable of byte chunks."""
    buf = bytearray()
    for chunk in chunks:
        buf += chunk
        while True:
            start = buf.find(SYNC)
            if start < 0:
                buf.clear()
                break
            del buf[:start]
            if len(buf) < 3:
                break
            length = buf[2]
            if not 3 <= length <= 250:
                del buf[0]
                continue
            end = 3 + length
            if len(buf) < end:
                break
            crc = buf[end - 2] | buf[end - 1] << 8
            if binascii.crc_hqx(bytes(buf[1:end - 2]), 0xFFFF) != crc:
                del buf[0]  # a false sync inside data, or corruption
                continue
            if buf[1] == 1 and buf[3] == 0x01 and length >= 76:
                seq, time_us, period_us, flags = struct.unpack_from("<BIHH", buf, 4)
                channels = struct.unpack_from("<32h", buf, 13)
                yield seq, time_us, period_us, flags, channels
            # other versions and types: skipped by their length
            del buf[:end]
```

Reading the radio with [pyserial](https://pypi.org/project/pyserial/) (`pip install pyserial`):

```python
import serial

with serial.Serial("/dev/ttyACM0") as port:  # "COM5" on Windows
    chunks = iter(lambda: port.read(port.in_waiting or 1), b"")
    for seq, time_us, period_us, flags, ch in frames(chunks):
        pulses = [1500 + v // 2 for v in ch]  # microseconds
        print(seq, period_us, pulses[:4])
```

A channel value of −1024 to +1024 is −100 % to +100 %, and extended limits reach ±1536 (±150 %). The pulse width in microseconds is 1500 + value / 2.

For the lowest delay:

- Read the port continuously in a thread of its own, parse every complete frame, and hand only the newest one to the rest of the application.
- Never read more slowly than the radio sends. The operating system buffers whatever is not read, and an application that falls behind gets older and older frames.
- Drop the first frame after opening the port: it may have waited in the radio's USB endpoint while nobody was reading.
- Connect the radio directly to the computer, not through a hub (see [speed-improvements.md](speed-improvements.md)).

## Building

The steps are the usual EdgeTX firmware build. EdgeTX 2.11 and newer require exactly version 14.2.1 of the Arm GNU toolchain; pass its directory with `ARM_TOOLCHAIN_DIR`:

```sh
cmake -S . -B build-tx16s -DARM_TOOLCHAIN_DIR=/opt/gcc-arm-none-eabi/bin \
      -DPCB=X10 -DPCBREV=TX16S -DCMAKE_BUILD_TYPE=Release
cmake --build build-tx16s --target arm-none-eabi-configure
cmake --build build-tx16s/arm-none-eabi --target firmware --parallel
```

The image is `build-tx16s/arm-none-eabi/firmware.bin`. Building `arm-none-eabi` directly runs make in parallel; building through the top-level directory runs it single-threaded. If the toolchain changes, delete the build directory instead of reconfiguring it.

| Radio | Options |
| --- | --- |
| RadioMaster TX16S | `-DPCB=X10 -DPCBREV=TX16S` |
| RadioMaster TX12 MK2 | `-DPCB=X7 -DPCBREV=TX12MK2` |
| RadioMaster TX12 (first version) | `-DPCB=X7 -DPCBREV=TX12 -DUSB_SERIAL=ON` |
| RadioMaster GX12 (2.11 and newer) | `-DPCB=X7 -DPCBREV=GX12` |

The first TX12 needs `-DUSB_SERIAL=ON`: its default build, like the official EdgeTX firmware for it, has no USB serial port at all, so the Channels mode would not exist.

v2.10.7 does not configure with CMake 4; use CMake 3.x, for example from `pip install "cmake<4"`.

## Other change: a mode hidden by a port the radio does not have

EdgeTX hides a serial mode from every port once any port uses it. It checked every port number, including ports the radio does not have. A `radio.yml` copied from a radio that has an AUX2 port, with a mode such as LUA set on AUX2, therefore hid that mode from USB-VCP on a radio without AUX2, where no AUX2 row can be shown to clear it. `serialGetModePort()` now skips ports the radio does not have.

## Testing status

Done:

- On a Radiomaster GX12 with the `channel-stream` branch: 1000.0 frames per second with RF off, 0.997–1.003 ms between frames on the radio's clock, no bad frames and no skipped mixer runs over 100 s of captures.
- On every branch: the stream and USB code compiled for a PC against a simulated USB endpoint that takes each transfer's bytes at the last moment the hardware could, so data overwritten while in flight would show up as corruption; a 20-second 1 kHz session with random host stalls decodes exactly.
- On the release branches: firmware builds for the TX16S, TX12 MK2 and TX12, and EdgeTX's own unit tests pass for the TX16S and TX12.

Not yet checked:

- the release branches on a radio;
- the frame rate with an RF module on;
- Windows;
- the Companion part of the change, which has not been compiled.
