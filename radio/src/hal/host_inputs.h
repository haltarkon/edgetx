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

// Host inputs (HOST_INPUTS): controls that are not wired to the radio but
// handed to it by the program that hosts it -- the axes and buttons of a game
// controller, a mouse, the keys of a keyboard. They are mixer sources and
// switch sources of their own, beside the radio's sticks, pots and switches:
//
//   axes     MIXSRC_FIRST_HOST_AXIS..    -1024..1024, as the host reports them
//   buttons  SWSRC_FIRST_HOST_BUTTON..   on while pressed (the host may latch)
//            MIXSRC_FIRST_HOST_BUTTON..  the first MAX_HOST_ANALOG_BUTTONS of
//                                        them also read as sources, -1024..1024
//                                        (how far an analog button is pressed)
//
// Which control each index is, and what it is called in a model file, is the
// board's business: it implements the functions below. Names are the tokens
// the model YAML reads and writes and must not collide with any other source
// or switch token; labels are what a source or switch list shows.

#if defined(HOST_INPUTS)

#include <stddef.h>
#include <stdint.h>

// Sizes of the source ranges. Model fields hold a source or a switch in 10
// bits (+-511), which is what bounds them: see the static_asserts in
// dataconstants.h.
#if !defined(MAX_HOST_AXES)
#define MAX_HOST_AXES 27
#endif
#if !defined(MAX_HOST_BUTTONS)
#define MAX_HOST_BUTTONS 126
#endif
#if !defined(MAX_HOST_ANALOG_BUTTONS)
#define MAX_HOST_ANALOG_BUTTONS 24
#endif

// How many of each the board has (<= the maxima above).
uint8_t hostInputsGetMaxAxes();
uint8_t hostInputsGetMaxButtons();

int16_t hostAxisGetValue(uint8_t idx);    // -1024..1024, 0 for an axis it lacks
bool hostButtonGetState(uint8_t idx);     // false for a button it lacks
int16_t hostButtonGetValue(uint8_t idx);  // -1024 released .. 1024 pressed

const char* hostAxisGetName(uint8_t idx);  // nullptr for an axis it lacks
const char* hostAxisGetLabel(uint8_t idx);
int hostAxisLookupIdx(const char* name, size_t len);  // -1 when unknown

const char* hostButtonGetName(uint8_t idx);
const char* hostButtonGetLabel(uint8_t idx);
int hostButtonLookupIdx(const char* name, size_t len);

#endif  // HOST_INPUTS
