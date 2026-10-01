// SPDX-License-Identifier: GPL-2.0-or-later
//
// The glue's side of the virtual radio's host inputs (etx_host_inputs.cpp);
// EdgeTX's side is radio/src/hal/host_inputs.h.

#pragma once

#if defined(HOST_INPUTS)

#include <stdint.h>

// Every axis centred, every button released.
void etxHostInputsReset();

// -1024..1024 (clamped); false for an index the radio lacks. A button is
// pressed while its value is above zero.
bool etxHostSetAxis(int32_t index, int32_t value);
bool etxHostSetButton(int32_t index, int32_t value);

// What an index is, for etx_describe_json: "pad" | "mouse" (| "key" for a
// button), and its position among the controls of that kind (a controller's
// axis or button number, MouseEvent.button, the position in the key table).
const char* etxHostAxisKind(int32_t index);
const char* etxHostButtonKind(int32_t index);
int32_t etxHostAxisOrdinal(int32_t index);
int32_t etxHostButtonOrdinal(int32_t index);

#endif
