// SPDX-License-Identifier: GPL-2.0-or-later
//
// The virtual radio's host inputs (radio/src/hal/host_inputs.h): which control
// each host axis and button is, what a model file calls it, and the values the
// host last reported.
//
// The virtual radio has no pots and no switches of its own. What a pilot holds
// instead is whatever the hosting program can read -- a game controller, a
// mouse, a keyboard -- and each of those controls is a source of the mixer in
// its own right:
//
//   axes     Axis0..Axis23            a controller's axes, numbered as it numbers
//                                     them (the Gamepad API's axes[0]..)
//            MouseX, MouseY,          the pointer and the wheel, as positions
//            MouseWheel               the host integrates from their movement
//   buttons  Btn0..Btn23              a controller's buttons, numbered as it
//                                     numbers them; as a source, how far each
//                                     is pressed
//            MouseLeft..Mouse5        MouseEvent.button 0..4
//            KeyA, Digit1, Space...   keyboard keys by KeyboardEvent.code, the
//                                     physical key whatever the layout
//
// Names are the model YAML's tokens. A key's token is its KeyboardEvent.code,
// so the host needs no table of its own to find the index of a key event:
// etx_describe_json lists every control with its index.

#if defined(HOST_INPUTS)

#include "etx_host_inputs.h"

#include "hal/host_inputs.h"

#include <stdio.h>
#include <string.h>

namespace {

constexpr int PAD_AXES = 24;
constexpr int PAD_BUTTONS = 24;

struct NamedInput {
  const char* name;
  const char* label;
};

const NamedInput kMouseAxes[] = {
    {"MouseX", "Mouse X"},
    {"MouseY", "Mouse Y"},
    {"MouseWheel", "Wheel"},
};

const NamedInput kMouseButtons[] = {
    {"MouseLeft", "Mouse L"},  {"MouseMiddle", "Mouse M"}, {"MouseRight", "Mouse R"},
    {"Mouse4", "Mouse 4"},     {"Mouse5", "Mouse 5"},
};

// KeyboardEvent.code values (UI Events KeyboardEvent code Values). Left out:
// Escape and Tab (the host keeps them for itself), and Control, Alt and Meta,
// which a browser reports only as modifiers of another key.
const NamedInput kKeys[] = {
    {"KeyA", "Key A"}, {"KeyB", "Key B"}, {"KeyC", "Key C"}, {"KeyD", "Key D"},
    {"KeyE", "Key E"}, {"KeyF", "Key F"}, {"KeyG", "Key G"}, {"KeyH", "Key H"},
    {"KeyI", "Key I"}, {"KeyJ", "Key J"}, {"KeyK", "Key K"}, {"KeyL", "Key L"},
    {"KeyM", "Key M"}, {"KeyN", "Key N"}, {"KeyO", "Key O"}, {"KeyP", "Key P"},
    {"KeyQ", "Key Q"}, {"KeyR", "Key R"}, {"KeyS", "Key S"}, {"KeyT", "Key T"},
    {"KeyU", "Key U"}, {"KeyV", "Key V"}, {"KeyW", "Key W"}, {"KeyX", "Key X"},
    {"KeyY", "Key Y"}, {"KeyZ", "Key Z"},
    {"Digit1", "Key 1"}, {"Digit2", "Key 2"}, {"Digit3", "Key 3"}, {"Digit4", "Key 4"},
    {"Digit5", "Key 5"}, {"Digit6", "Key 6"}, {"Digit7", "Key 7"}, {"Digit8", "Key 8"},
    {"Digit9", "Key 9"}, {"Digit0", "Key 0"},
    {"Backquote", "Key `"}, {"Minus", "Key -"}, {"Equal", "Key ="},
    {"BracketLeft", "Key ["}, {"BracketRight", "Key ]"}, {"Backslash", "Key \\"},
    {"Semicolon", "Key ;"}, {"Quote", "Key '"}, {"Comma", "Key ,"},
    {"Period", "Key ."}, {"Slash", "Key /"}, {"IntlBackslash", "Key <>"},
    {"Space", "Space"}, {"Enter", "Enter"}, {"Backspace", "Backspace"},
    {"ShiftLeft", "L Shift"}, {"ShiftRight", "R Shift"}, {"CapsLock", "Caps Lock"},
    {"ArrowUp", "Key \xe2\x86\x91"}, {"ArrowDown", "Key \xe2\x86\x93"},
    {"ArrowLeft", "Key \xe2\x86\x90"}, {"ArrowRight", "Key \xe2\x86\x92"},
    {"Insert", "Insert"}, {"Delete", "Delete"}, {"Home", "Home"}, {"End", "End"},
    {"PageUp", "Page Up"}, {"PageDown", "Page Down"},
    {"F1", "F1"}, {"F2", "F2"}, {"F3", "F3"}, {"F4", "F4"}, {"F5", "F5"}, {"F6", "F6"},
    {"F7", "F7"}, {"F8", "F8"}, {"F9", "F9"}, {"F10", "F10"}, {"F11", "F11"}, {"F12", "F12"},
    {"Numpad0", "Num 0"}, {"Numpad1", "Num 1"}, {"Numpad2", "Num 2"}, {"Numpad3", "Num 3"},
    {"Numpad4", "Num 4"}, {"Numpad5", "Num 5"}, {"Numpad6", "Num 6"}, {"Numpad7", "Num 7"},
    {"Numpad8", "Num 8"}, {"Numpad9", "Num 9"},
    {"NumpadDecimal", "Num ."}, {"NumpadAdd", "Num +"}, {"NumpadSubtract", "Num -"},
    {"NumpadMultiply", "Num *"}, {"NumpadDivide", "Num /"}, {"NumpadEnter", "Num Enter"},
    {"PrintScreen", "Print Screen"}, {"ScrollLock", "Scroll Lock"}, {"Pause", "Pause"},
    {"NumLock", "Num Lock"}, {"ContextMenu", "Menu"},
};

template <typename T, size_t N>
constexpr int dim(const T (&)[N])
{
  return (int)N;
}

constexpr int MOUSE_AXES = dim(kMouseAxes);
constexpr int MOUSE_BUTTONS = dim(kMouseButtons);
constexpr int KEYS = dim(kKeys);
constexpr int N_AXES = PAD_AXES + MOUSE_AXES;
constexpr int N_BUTTONS = PAD_BUTTONS + MOUSE_BUTTONS + KEYS;

static_assert(N_AXES == MAX_HOST_AXES, "MAX_HOST_AXES (hal/host_inputs.h) != the axis table");
static_assert(N_BUTTONS == MAX_HOST_BUTTONS,
              "MAX_HOST_BUTTONS (hal/host_inputs.h) != the button table");
static_assert(PAD_BUTTONS == MAX_HOST_ANALOG_BUTTONS,
              "MAX_HOST_ANALOG_BUTTONS (hal/host_inputs.h) != the controller's buttons");

// "Axis12" / "Axis 12", "Btn3" / "Btn 3": generated once, numbered from 0 as the host does.
struct Numbered {
  char name[PAD_AXES > PAD_BUTTONS ? PAD_AXES : PAD_BUTTONS][8];
  char label[PAD_AXES > PAD_BUTTONS ? PAD_AXES : PAD_BUTTONS][8];
  Numbered(const char* prefix, int count)
  {
    for (int i = 0; i < count; i++) {
      snprintf(name[i], sizeof(name[i]), "%s%d", prefix, i);
      snprintf(label[i], sizeof(label[i]), "%s %d", prefix, i);
    }
  }
};

const Numbered& padAxes()
{
  static const Numbered table("Axis", PAD_AXES);
  return table;
}

const Numbered& padButtons()
{
  static const Numbered table("Btn", PAD_BUTTONS);
  return table;
}

int16_t s_axes[N_AXES];
int16_t s_buttons[N_BUTTONS];

int16_t clampValue(int32_t value)
{
  return value < -1024 ? -1024 : value > 1024 ? 1024 : (int16_t)value;
}

bool same(const char* candidate, const char* name, size_t len)
{
  return candidate && strlen(candidate) == len && !memcmp(candidate, name, len);
}

}  // namespace

// ---- hal/host_inputs.h --------------------------------------------------------

uint8_t hostInputsGetMaxAxes() { return N_AXES; }
uint8_t hostInputsGetMaxButtons() { return N_BUTTONS; }

int16_t hostAxisGetValue(uint8_t idx) { return idx < N_AXES ? s_axes[idx] : 0; }
int16_t hostButtonGetValue(uint8_t idx) { return idx < N_BUTTONS ? s_buttons[idx] : -1024; }
bool hostButtonGetState(uint8_t idx) { return idx < N_BUTTONS && s_buttons[idx] > 0; }

const char* hostAxisGetName(uint8_t idx)
{
  if (idx < PAD_AXES) return padAxes().name[idx];
  if (idx < N_AXES) return kMouseAxes[idx - PAD_AXES].name;
  return nullptr;
}

const char* hostAxisGetLabel(uint8_t idx)
{
  if (idx < PAD_AXES) return padAxes().label[idx];
  if (idx < N_AXES) return kMouseAxes[idx - PAD_AXES].label;
  return nullptr;
}

int hostAxisLookupIdx(const char* name, size_t len)
{
  for (int i = 0; i < N_AXES; i++)
    if (same(hostAxisGetName(i), name, len)) return i;
  return -1;
}

const char* hostButtonGetName(uint8_t idx)
{
  if (idx < PAD_BUTTONS) return padButtons().name[idx];
  if (idx < PAD_BUTTONS + MOUSE_BUTTONS) return kMouseButtons[idx - PAD_BUTTONS].name;
  if (idx < N_BUTTONS) return kKeys[idx - PAD_BUTTONS - MOUSE_BUTTONS].name;
  return nullptr;
}

const char* hostButtonGetLabel(uint8_t idx)
{
  if (idx < PAD_BUTTONS) return padButtons().label[idx];
  if (idx < PAD_BUTTONS + MOUSE_BUTTONS) return kMouseButtons[idx - PAD_BUTTONS].label;
  if (idx < N_BUTTONS) return kKeys[idx - PAD_BUTTONS - MOUSE_BUTTONS].label;
  return nullptr;
}

int hostButtonLookupIdx(const char* name, size_t len)
{
  for (int i = 0; i < N_BUTTONS; i++)
    if (same(hostButtonGetName(i), name, len)) return i;
  return -1;
}

// ---- the glue's side ------------------------------------------------------------

void etxHostInputsReset()
{
  memset(s_axes, 0, sizeof(s_axes));
  for (int i = 0; i < N_BUTTONS; i++) s_buttons[i] = -1024;
}

bool etxHostSetAxis(int32_t index, int32_t value)
{
  if (index < 0 || index >= N_AXES) return false;
  s_axes[index] = clampValue(value);
  return true;
}

bool etxHostSetButton(int32_t index, int32_t value)
{
  if (index < 0 || index >= N_BUTTONS) return false;
  s_buttons[index] = clampValue(value);
  return true;
}

const char* etxHostAxisKind(int32_t index) { return index < PAD_AXES ? "pad" : "mouse"; }

const char* etxHostButtonKind(int32_t index)
{
  return index < PAD_BUTTONS ? "pad" : index < PAD_BUTTONS + MOUSE_BUTTONS ? "mouse" : "key";
}

int32_t etxHostAxisOrdinal(int32_t index) { return index < PAD_AXES ? index : index - PAD_AXES; }

int32_t etxHostButtonOrdinal(int32_t index)
{
  if (index < PAD_BUTTONS) return index;
  if (index < PAD_BUTTONS + MOUSE_BUTTONS) return index - PAD_BUTTONS;
  return index - PAD_BUTTONS - MOUSE_BUTTONS;
}

#endif  // HOST_INPUTS
