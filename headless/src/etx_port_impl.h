// SPDX-License-Identifier: GPL-2.0-or-later
//
// Internal interfaces shared by the headless mixer's glue files
// (etx_port.cpp, etx_fs.cpp, etx_board.cpp, etx_api.cpp).

#pragma once

#include <stdint.h>

// Virtual clock (etx_port.cpp): what time_get_ms() and friends report.
uint32_t etxVirtualMs();
void etxAdvanceVirtualMs(uint32_t ms);
void etxSetVirtualMs(uint32_t ms);

// Memory files (etx_fs.cpp) behind the FatFs API.
void etxFsPut(const char* path, const uint8_t* data, uint32_t len);
bool etxFsGet(const char* path, const uint8_t** data, uint32_t* len);
void etxFsRemove(const char* path);

// Virtual board (etx_board.cpp).
// Analog inputs are raw ADC values 0..4096 in the radio's ADC input order.
void etxBoardInit();
void etxBoardSetAnalog(uint8_t index, uint16_t raw);
uint16_t etxBoardGetAnalog(uint8_t index);
void etxBoardSetSwitch(uint8_t index, int8_t position);
int8_t etxBoardGetSwitch(uint8_t index);
void etxBoardSetKey(uint8_t key, bool pressed);
void etxBoardSetTrimKey(uint8_t trimSwitch, bool pressed);
void etxBoardReleaseAll();

// Event log (etx_api.cpp): one JSON object per line, drained by etx_poll_events.
void etxEventf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
