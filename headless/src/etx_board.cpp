// SPDX-License-Identifier: GPL-2.0-or-later
//
// Board support for the headless mixer: a virtual radio whose sticks, pots,
// switches, keys and trim buttons are set by the host through the API.
//
// It is to this build what targets/simu is to EdgeTX's simulator: the lowest
// layer, below EdgeTX's own HAL (hal/adc_driver.cpp, hal/switch_driver.cpp,
// keys.cpp), which reads through it exactly as it reads the real hardware. The
// radio's hardware tables are EdgeTX's, generated from its JSON hardware
// definition (hal_adc_inputs.inc, simu_switches.inc).
//
// The virtual radio (ETX_VIRTUAL_RADIO, cmake/inject.cmake) is the exception:
// a radio that exists only here. It is built with a real radio's configuration
// (so every size and every model field is one EdgeTX already ships) but with
// the hardware tables below instead of the generated ones: two gimbals with
// their trim buttons, and no pots or switches at all. What a pilot holds
// besides the sticks are the host's own controls -- controller axes and
// buttons, a mouse, keyboard keys -- which reach the mixer as host inputs
// (etx_host_inputs.cpp, radio/src/hal/host_inputs.h).
//
// The simulator's own drivers are not linked because they carry the
// simulator's WASM export/import attributes (targets/simu/simulib.h), which
// would export that API from this module and keep its whole firmware alive.

#include "etx_host_inputs.h"
#include "etx_port_impl.h"

#include "edgetx.h"
#include "hal/adc_driver.h"
#include "hal/switch_driver.h"
#include "hal/usb_driver.h"
#include "switches.h"

#include <string.h>

// ---- analog inputs ----------------------------------------------------------

#if defined(ETX_VIRTUAL_RADIO)
// As hal_adc_inputs.inc is generated: name, canonical (YAML) name, label.
static const etx_hal_adc_input_t _main_inputs[] = {
    {"LH", "Rud", STR_STICK_NAMES0},
    {"LV", "Ele", STR_STICK_NAMES1},
    {"RV", "Thr", STR_STICK_NAMES2},
    {"RH", "Ail", STR_STICK_NAMES3},
};
static const etx_hal_adc_input_t _vbat_inputs[] = {{"VBAT", nullptr, nullptr}};
static const etx_hal_adc_input_t _rtc_bat_inputs[] = {{"RTC_BAT", nullptr, nullptr}};

// { count, offset, inputs } for main, flex, vbat, rtc_bat, lux, then all.
static const etx_hal_adc_inputs_t _hal_inputs[] = {
    {4, 0, _main_inputs}, {0, 4, nullptr}, {1, 4, _vbat_inputs},
    {1, 5, _rtc_bat_inputs}, {0, 6, nullptr}, {6, 0, nullptr},
};
constexpr potconfig_t _pot_default_config = 0;
#else
#include "hal_adc_inputs.inc"
#endif

static uint16_t s_raw[MAX_ANALOG_INPUTS];

void etxBoardSetAnalog(uint8_t index, uint16_t raw)
{
  if (index < MAX_ANALOG_INPUTS) s_raw[index] = raw > 4096 ? 4096 : raw;
}

uint16_t etxBoardGetAnalog(uint8_t index)
{
  return index < MAX_ANALOG_INPUTS ? s_raw[index] : 0;
}

// The radio reports its battery half a volt above the warning threshold, as
// the simulator does, so no low-battery alarm ever fires.
static uint16_t defaultBatteryDeciVolts()
{
  uint16_t warn = BATTERY_WARN;
  if (g_eeGeneral.vBatWarn > 0) warn = g_eeGeneral.vBatWarn;
  return warn + 5;
}

static bool etx_adc_start_conversion()
{
  int n = adcGetMaxInputs(ADC_INPUT_ALL);
  for (int i = 0; i < n; i++) setAnalogValue(i, s_raw[i]);
  int vbat = adcGetInputOffset(ADC_INPUT_VBAT);
  if (adcGetMaxInputs(ADC_INPUT_VBAT) > 0)
    setAnalogValue(vbat, defaultBatteryDeciVolts() * 10 * 2);
  return true;
}

const etx_hal_adc_driver_t etx_adc_driver = {
  .inputs = _hal_inputs,
  .default_pots_cfg = _pot_default_config,
  .init = nullptr,
  .start_conversion = etx_adc_start_conversion,
  .wait_completion = nullptr,
};

void enableVBatBridge() {}
void disableVBatBridge() {}
bool isVBatBridgeEnabled() { return false; }

uint16_t getBatteryVoltage()
{
  if (adcGetMaxInputs(ADC_INPUT_VBAT) < 1) return defaultBatteryDeciVolts() * 10;
  return anaIn(adcGetInputOffset(ADC_INPUT_VBAT));
}

uint16_t getRTCBatteryVoltage() { return 300; }

uint16_t getLuxSensorValue()
{
  if (adcGetMaxInputs(ADC_INPUT_LUX) < 1) return 0;
  return anaIn(adcGetInputOffset(ADC_INPUT_LUX));
}

// ---- switches -----------------------------------------------------------------

struct hw_switch_def {
  const char* name;
  SwitchHwType type;
  SwitchConfig defaultType;
#if defined(FUNCTION_SWITCHES)
  bool isCustomSwitch;
  uint8_t customSwitchIdx;
#endif
};

#if defined(ETX_VIRTUAL_RADIO)
// No switches: the entry only gives the table a size.
const hw_switch_def _switch_defs[] = {{"", SWITCH_HW_2POS, SWITCH_NONE}};
constexpr uint8_t n_switches = 0;
#else
#include "simu_switches.inc"
#endif

// -1 up, 0 middle, +1 down: the convention of EdgeTX's simuSetSwitch().
static int8_t s_switches[MAX_SWITCHES];

void etxBoardSetSwitch(uint8_t index, int8_t position)
{
  if (index < MAX_SWITCHES) s_switches[index] = position < 0 ? -1 : position > 0 ? 1 : 0;
}

int8_t etxBoardGetSwitch(uint8_t index)
{
  return index < MAX_SWITCHES ? s_switches[index] : -1;
}

void boardInitSwitches() { memset(s_switches, -1, sizeof(s_switches)); }

#if defined(RADIO_GX12)
void _poll_switches() {}
#endif

SwitchHwPos boardSwitchGetPosition(uint8_t idx)
{
  if (idx >= MAX_SWITCHES || s_switches[idx] < 0) return SWITCH_HW_UP;
  return s_switches[idx] == 0 ? SWITCH_HW_MID : SWITCH_HW_DOWN;
}

const char* boardSwitchGetName(uint8_t idx)
{
  return idx < n_switches ? _switch_defs[idx].name : nullptr;
}
SwitchHwType boardSwitchGetType(uint8_t idx)
{
  return idx < n_switches ? _switch_defs[idx].type : SWITCH_HW_2POS;
}
uint8_t boardGetMaxSwitches() { return n_switches; }
SwitchConfig boardSwitchGetDefaultConfig(uint8_t idx)
{
  return idx < n_switches ? _switch_defs[idx].defaultType : SWITCH_NONE;
}

#if defined(FUNCTION_SWITCHES)
bool boardIsCustomSwitch(uint8_t idx)
{
  return idx < n_switches ? _switch_defs[idx].isCustomSwitch : false;
}
uint8_t boardGetCustomSwitchIdx(uint8_t idx) { return _switch_defs[idx].customSwitchIdx; }
#endif

#if !defined(COLORLCD)
switch_display_pos_t switchGetDisplayPosition(uint8_t idx)
{
  if (idx >= DIM(_switch_display)) return {0, 0};
  return _switch_display[idx];
}
#endif

// ---- keys and trim buttons -------------------------------------------------

static bool s_keys[MAX_KEYS];
static bool s_trims[MAX_TRIMS * 2];

void etxBoardSetKey(uint8_t key, bool pressed)
{
  if (key < MAX_KEYS) s_keys[key] = pressed;
}

void etxBoardSetTrimKey(uint8_t trimSwitch, bool pressed)
{
  if (trimSwitch < MAX_TRIMS * 2) s_trims[trimSwitch] = pressed;
}

void etxBoardReleaseAll()
{
  memset(s_keys, 0, sizeof(s_keys));
  memset(s_trims, 0, sizeof(s_trims));
}

uint32_t readKeys()
{
  uint32_t result = 0;
  for (int i = 0; i < MAX_KEYS; i++)
    if (s_keys[i]) result |= 1u << i;
  return result;
}

uint32_t readTrims()
{
  uint32_t result = 0;
  for (int i = 0; i < keysGetMaxTrims() * 2; i++)
    if (s_trims[i]) result |= 1u << i;
  return result;
}

void pollKeys() {}

#if defined(TRIMS_EMULATE_BUTTONS)
static bool s_hatsAsKeys = false;
void setHatsAsKeys(bool val) { s_hatsAsKeys = val; }
bool getHatsAsKeys() { return s_hatsAsKeys; }
#endif

// ---- the rest of the board: always on, nothing attached --------------------

void etxBoardInit()
{
  // Every stick and pot starts centred (2048 on the 0..4096 scale).
  for (int i = 0; i < MAX_ANALOG_INPUTS; i++) s_raw[i] = 2048;
  etxBoardReleaseAll();
#if defined(HOST_INPUTS)
  etxHostInputsReset();
#endif
  adcInit(&etx_adc_driver);
  switchInit();
}

void boardInit() { switchInit(); }
void boardOff() {}

uint32_t pwrCheck() { return e_power_on; }
bool pwrPressed() { return false; }
bool pwrOffPressed() { return false; }
void pwrInit() {}
void pwrOn() {}
void pwrOff() {}
bool UNEXPECTED_SHUTDOWN() { return false; }
void SET_POWER_REASON(uint32_t) {}

int usbPlugged() { return false; }
int getSelectedUsbMode() { return USB_JOYSTICK_MODE; }
void setSelectedUsbMode(int) {}

void delay_ms(uint32_t) {}
void delay_us(uint16_t) {}

// Haptic output. EdgeTX's haptic queue drives these from its own timing;
// the API reports haptic requests from HapticQueue itself (see etx_api.cpp).
uint32_t simuHapticValue = 0;
void hapticOn(uint32_t) {}
void hapticOff() {}

// ---- the rest of the simulator's board functions --------------------------------
// Linked code refers to these; none of it is on the mixer's path.

#include "hal/rotary_encoder.h"
#include "hal/serial_port.h"
#include "rtc.h"

rotenc_t rotaryEncoderGetValue() { return 0; }

#if !defined(COLORLCD)
void lcdSetRefVolt(uint8_t) {}
#endif
#if LCD_W == 128
void lcdSetInvert(bool) {}
#endif

bool storageIsPresent() { return true; }
uint32_t isBootloaderStart(const uint8_t*) { return 1; }
void flashWrite(uint32_t*, const uint32_t*) {}

#if defined(USB_SERIAL)
const etx_serial_port_t UsbSerialPort = {"USB-VCP", nullptr, nullptr};
#endif
const etx_serial_port_t* auxSerialGetPort(int) { return nullptr; }

void rtcInit() {}
void rtcGetTime(struct gtm*) {}
uint16_t rtcGetTimeMs(struct gtm*) { return 0; }
void rtcDriverSetTime(const struct gtm*) {}
int32_t rtcGetCalibration() { return 0; }
void rtcSetCalibration(int32_t) {}
gtime_t rtcGetCalibrationRef() { return 0; }
void rtcSetCalibrationRef(gtime_t) {}
void rtcClearCalibrationRef() {}

// Colour builds: the simulator's touch panel and rotary encoder state.
__attribute__((weak)) bool simu_shutdown = false;
__attribute__((weak)) volatile uint32_t rotencDt = 0;

#if defined(HARDWARE_TOUCH)
#include "touch.h"
struct TouchState touchPanelRead() { return TouchState{}; }
bool touchPanelEventOccured() { return false; }
struct TouchState getInternalTouchState() { return TouchState{}; }
#endif

#if defined(COLORLCD)
// Colour builds leave the GUI out of the link (cmake/inject.cmake); the mixer
// reads whether the calibration page is open (evalInputs(), centre beeps).
uint8_t menuCalibrationState = 0;
#endif
