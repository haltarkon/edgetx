# Speed improvements

Everything in these branches that makes channel data reach the computer sooner. Changes 1–4 are in the USB serial driver; changes 1 and 3 speed up everything sent over USB-VCP, including Lua scripts that call `serialWrite()`. Changes 5–8 are the channel stream itself.

## Where the time went

From a stick movement to an application on the computer, before and after:

| Stage | Lua script over USB-VCP, stock EdgeTX | Channels mode |
| --- | --- | --- |
| Mixer period | 4 ms | 1 ms with RF modules off, otherwise the module's period |
| Mixer output to send | 0–10 ms: `getTime()` ticks in 10 ms, and a script's `run()` starts every 50 ms | none: sent right after the mixer run |
| Send to USB transfer | 0–16 ms, sometimes 32 ms: VCP data left the radio on every 16th USB frame | ~0 ms: the transfer starts at once when the endpoint is idle |
| Host USB and serial driver | ~1 ms | ~1 ms |
| Total | ~15 ms typical, up to ~45 ms | ~1.5 ms typical, up to ~3 ms |

The totals are added up from the stages: measuring the whole path end to end needs an external reference. The measured parts are at the end of this page.

## The changes

### USB serial driver

Files: `radio/src/targets/common/arm/stm32/usbd_cdc.cpp`, `radio/src/targets/common/arm/stm32/usbd_conf.h`.

1. **Data leaves on every USB frame instead of every 16th.** `CDC_IN_FRAME_INTERVAL` goes from 15 to 0 (from 4 to 0 on STM32F2 radios). Data written byte by byte, which is how Lua's `serialWrite()`, the CLI and the telemetry mirror write, used to wait for every 16th USB start of frame, up to 16 ms (every 5th, up to 5 ms, on STM32F2). Now it is picked up at the next 1 ms frame.
2. **A transfer starts as soon as data is written.** The driver gains `sendBuffer()`, which copies a whole buffer into the transmit queue and, if the endpoint is idle, starts the USB transfer right away instead of at the next start of frame.
3. **Transfers are chained.** When a transfer completes, whatever was queued meanwhile is sent at once. Before, it waited for the next start-of-frame slot, up to 16 ms later.
4. **Whole frames only.** `sendBuffer()` queues a buffer completely or not at all, so the host never receives part of a frame and waits for the rest. Its free-space check also leaves out the bytes the endpoint is still sending, so new data never overwrites data in flight. A new `txCompleted()` reports when the host has taken everything queued; change 7 uses it.

### Channel stream

Files: `radio/src/channel_stream.{h,cpp}`, `radio/src/tasks/mixer_task.cpp`, `radio/src/mixer_scheduler.cpp`.

5. **Sent by the mixer task, right after the mixer run.** The frame is built and sent right after each mixer run's RF pulses, in the task with the highest priority. Nothing waits for a Lua `run()` callback (every 50 ms) or a `getTime()` tick (10 ms) between sampling and sending. `time_us` is taken just before the mixer reads the sticks.
6. **The mixer runs at 1 kHz while the stream is active.** With the USB port in serial mode, the stream attached and no RF module setting the period, the mixer period drops from 4 ms to 1 ms, as Joystick mode already does. With an RF module active, the module keeps its period, and the stream follows it.
7. **Always the newest sample, never a backlog.** While the host has not taken the previous frame, the radio skips the frame instead of queueing it, so the host gets fresh data the moment it reads again instead of a queue of old samples. `seq` counts mixer runs, so the host can see the skipped runs.
8. **Fixed binary frames.** One 79-byte frame carries all 32 channels from one mixer run, so the application gets a consistent snapshot. Building it with its CRC takes a few microseconds; the host needs no text parsing, and every frame has the same length.

## Measured

On a Radiomaster GX12 with the `channel-stream` branch, connected to Linux:

- **Channels mode, RF off, 100 s of captures:** 1000.0 frames per second; 0.997–1.003 ms between frames on the radio's clock; no bad frames, no skipped mixer runs. Relative to the fastest path, half of the frames arrived within 0.12 ms and 99 % within 0.9 ms.
- **Through a USB 2.0 hub,** frames arrived bunched with a 5–7 ms gap every 4–5 s. The radio had sent them on time, so the hub added the delay. Plugged into a port on the computer itself, no gap reached 5 ms (the longest was 4.7 ms). Connect the radio without a hub.
- **An unchanged Lua script that sends a text packet every 10 ms over USB-VCP,** 30 s with a stick sweep, compared with stock EdgeTX 2.11.5 on the same radio. Lua writes byte by byte, so only changes 1 and 3 apply:

  | | Stock 2.11.5 | This firmware |
  | --- | --- | --- |
  | Interval between packets p50 / p99 / max | 16 / 32 / 33 ms | 10 / 17 / 17 ms |
  | Gaps of 30 ms or more | 75 | 0 |
  | Packets per USB transfer | 2 | 1 (10 % in pairs) |

  The intervals of up to 17 ms that remain come from the script's 50 ms callback, not from USB. Channels mode removes that as well.

## Cost

On v2.12.4 the whole change adds 512 bytes of flash on the TX16S and 552 bytes on the TX12 MK2. Building a frame with its CRC takes a few microseconds of the mixer task, so the user interface is unaffected.
