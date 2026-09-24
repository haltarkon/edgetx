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

#include "channel_stream.h"

#include "edgetx.h"
#include "mixer_scheduler.h"

static const etx_serial_driver_t* volatile streamDrv = nullptr;
static void* volatile streamCtx = nullptr;
static uint8_t streamSeq = 0;

void channelStreamSetSerialDriver(void* ctx, const etx_serial_driver_t* drv)
{
  // Same pattern as cliSetSerialDriver(): the driver is cleared first, so
  // the mixer task never pairs a new driver with a stale context.
  streamDrv = nullptr;
  streamCtx = ctx;
  streamDrv = drv;
}

bool channelStreamActive() { return streamDrv != nullptr; }

void channelStreamSend(uint32_t sampleTimeUs)
{
  // Counts every mixer run, so a gap on the host shows skipped samples
  uint8_t seq = streamSeq++;

  auto drv = streamDrv;
  if (!drv) return;
  auto ctx = streamCtx;

  // Sending behind a frame the host has not taken yet would only queue old
  // samples; the next mixer run brings a fresher one.
  if (drv->txCompleted && !drv->txCompleted(ctx)) return;

  uint8_t frame[CHANNEL_STREAM_FRAME_LEN];
  channelStreamBuildFrame(frame, seq, sampleTimeUs, getMixerSchedulerPeriod(),
                          channelOutputs, MAX_OUTPUT_CHANNELS);

  if (drv->sendBuffer) {
    drv->sendBuffer(ctx, frame, sizeof(frame));
  } else if (drv->sendByte) {
    for (uint8_t b : frame) drv->sendByte(ctx, b);
  }
}
