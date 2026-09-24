/*
 * Copyright (C) EdgeTX
 *
 * Based on code named
 *   opentx - https://github.com/opentx/opentx
 *   th9x - http://code.google.com/p/th9x
 *   er9x - http://code.google.com/p/er9x
 *   gruvin9x - http://code.google.com/p/gruvin9x
 *
 * License GPLv2: http://www.gnu.org/licenses/gpl-2.0.html
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#pragma once

#include <stdint.h>

#include "crc.h"
#include "hal/serial_driver.h"

// Channel stream: a serial port in UART_MODE_CHANNELS carries one binary
// frame per mixer run with all channel outputs, for a host application.
//
// Frame (little-endian, fixed size for each type):
//
//   offset size
//     0     1   sync      0xEC
//     1     1   version   protocol version, 1
//     2     1   len       bytes after this one (type, payload, crc)
//     3     1   type      0x01 = channels
//     4     1   seq       mixer run counter, wraps; a gap means skipped runs
//     5     4   time_us   radio microsecond clock when the mixer run started
//     9     2   period_us mixer period
//    11     2   flags     reserved, 0
//    13    64   ch[32]    int16 channel outputs as getOutputValue() returns
//                         them: -1024..1024 is -100..100 %
//    77     2   crc       CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF)
//                         over bytes 1..76 (version to the last channel)
//
// Every version keeps sync, version and len at offsets 0-2 and the same CRC,
// so a reader can check and skip a frame of a version it does not know. The
// version changes only when a reader of the previous version would misread a
// frame: fields appended to a type, with a longer len, keep it. Readers skip
// frames of unknown type by their length and must ignore set flag bits they
// do not know.

#define CHANNEL_STREAM_SYNC           0xEC
#define CHANNEL_STREAM_VERSION        1
#define CHANNEL_STREAM_TYPE_CHANNELS  0x01
#define CHANNEL_STREAM_CHANNELS       32
#define CHANNEL_STREAM_HEADER_LEN     13
#define CHANNEL_STREAM_FRAME_LEN \
  (CHANNEL_STREAM_HEADER_LEN + 2 * CHANNEL_STREAM_CHANNELS + 2)

// Baudrate of the port; USB-VCP ignores it
#define CHANNEL_STREAM_BAUDRATE       921600

inline void channelStreamPut16(uint8_t* p, uint16_t v)
{
  p[0] = v & 0xFF;
  p[1] = v >> 8;
}

// Writes one channels frame to `frame` (CHANNEL_STREAM_FRAME_LEN bytes).
// Channels beyond `count` are sent as 0.
inline void channelStreamBuildFrame(uint8_t* frame, uint8_t seq,
                                    uint32_t timeUs, uint16_t periodUs,
                                    const int16_t* channels, uint8_t count)
{
  frame[0] = CHANNEL_STREAM_SYNC;
  frame[1] = CHANNEL_STREAM_VERSION;
  frame[2] = CHANNEL_STREAM_FRAME_LEN - 3;
  frame[3] = CHANNEL_STREAM_TYPE_CHANNELS;
  frame[4] = seq;
  channelStreamPut16(frame + 5, timeUs & 0xFFFF);
  channelStreamPut16(frame + 7, timeUs >> 16);
  channelStreamPut16(frame + 9, periodUs);
  channelStreamPut16(frame + 11, 0);
  for (uint8_t i = 0; i < CHANNEL_STREAM_CHANNELS; i++) {
    channelStreamPut16(frame + CHANNEL_STREAM_HEADER_LEN + 2 * i,
                       i < count ? (uint16_t)channels[i] : 0);
  }
  uint16_t crc = crc16(CRC_1021, frame + 1, CHANNEL_STREAM_FRAME_LEN - 3,
                       0xFFFF);
  channelStreamPut16(frame + CHANNEL_STREAM_FRAME_LEN - 2, crc);
}

// Attach the port set to UART_MODE_CHANNELS (nullptr detaches it)
void channelStreamSetSerialDriver(void* ctx, const etx_serial_driver_t* drv);

// Is a port attached to the stream?
bool channelStreamActive();

// Called by the mixer task after each mixer run. Skips the frame while the
// previous one is still in transit, so the host never receives a queue of
// old samples.
void channelStreamSend(uint32_t sampleTimeUs);
