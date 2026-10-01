// SPDX-License-Identifier: GPL-2.0-or-later
//
// edgetx-headless: the C API of the headless EdgeTX mixer (see ../README.md).
//
// Every export here drives EdgeTX's own code: its YAML reader and writer, its
// model defaults, its analog/switch/trim pipeline, its mixer, logical
// switches, special functions, timers and trim handling. This file only
// decides *when* EdgeTX runs what — in the order the radio's 10 ms tick, mixer
// task and UI task run it — and translates between EdgeTX's globals and flat
// buffers the host can read.
//
// All exports are called from one thread, one at a time.

#include "etx_host_inputs.h"
#include "etx_json.h"
#include "etx_port_impl.h"

#include "edgetx.h"
#include "gvars.h"
#include "hal/adc_driver.h"
#include "hal/audio_driver.h"
#include "hal/switch_driver.h"
#include "input_mapping.h"
#include "mixes.h"
#include "model_init.h"
#include "storage/sdcard_common.h"
#include "storage/sdcard_yaml.h"
#include "storage/yaml/yaml_datastructs.h"
#include "storage/yaml/yaml_node.h"
#include "storage/yaml/yaml_parser.h"
#include "storage/yaml/yaml_tree_walker.h"
#include "switches.h"
#include "tasks/mixer_task.h"
#include "timers.h"
#include "translations/tts/tts.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#ifndef ETX_API_VERSION
#define ETX_API_VERSION 1
#endif
#ifndef ETX_EDGETX_COMMIT
#define ETX_EDGETX_COMMIT "unknown"
#endif
#ifndef ETX_TARGET_NAME
#define ETX_TARGET_NAME FLAVOUR
#endif

#if defined(ETX_NO_EXPORT_ATTR)  // size analysis builds: exports chosen at link time
#define ETX_EXPORT(name) extern "C"
#else
#define ETX_EXPORT(name) extern "C" __attribute__((export_name(#name), used))
#endif

// Where the API keeps the one model and the radio settings it loads through
// EdgeTX's file-based storage code (etx_fs.cpp holds them in memory).
static const char ETX_MODEL_FILE[] = "headless.yml";

// ---- errors -----------------------------------------------------------------

enum EtxError : int32_t {
  ETX_OK = 0,
  ETX_ERR_EMPTY = 1,        // no input
  ETX_ERR_NOT_A_MODEL = 2,  // parsed, but nothing in it matched the schema
  ETX_ERR_OVERFLOW = 3,     // a scalar longer than EdgeTX's parser buffer
  ETX_ERR_STORAGE = 4,      // EdgeTX's storage layer reported an error
  ETX_ERR_ARGUMENT = 5,
};

static std::string s_lastError;

static int32_t fail(int32_t code, const char* what)
{
  s_lastError = what ? what : "";
  return code;
}

// ---- events -------------------------------------------------------------------

static std::string s_events;
static const size_t EVENTS_MAX = 256 * 1024;

// One event line. It is formatted at whatever length it takes: a line cut to a
// fixed buffer would end inside a JSON string, and the host parses every line.
void etxEventf(const char* fmt, ...)
{
  char head[32];
  int n = snprintf(head, sizeof(head), "{\"t\":%u,", (unsigned)g_tmr10ms);
  va_list ap, again;
  va_start(ap, fmt);
  va_copy(again, ap);
  int m = vsnprintf(nullptr, 0, fmt, ap);
  va_end(ap);
  if (m < 0) {
    va_end(again);
    return;
  }
  std::string line(head, n);
  line.resize(n + m + 1);
  vsnprintf(&line[n], m + 1, fmt, again);
  va_end(again);
  line.resize(n + m);
  line += "}\n";
  if (s_events.size() + line.size() > EVENTS_MAX) {
    // Keep the newest: drop whole lines from the front, and say so where
    // they were, so the log still reads oldest first.
    size_t cut = s_events.find('\n', s_events.size() / 2);
    s_events.erase(0, cut == std::string::npos ? s_events.size() : cut + 1);
    char marker[48];
    int k = snprintf(marker, sizeof(marker), "{\"t\":%u,\"ev\":\"overflow\"}\n",
                     (unsigned)g_tmr10ms);
    s_events.insert(0, marker, k);
  }
  s_events += line;
}

static std::string jsonQuote(const char* s, size_t n = (size_t)-1)
{
  std::string out;
  etx::appendJsonString(out, s, n);
  return out;
}

// Numbers and durations reach the audio queue through the current language
// pack (translations/tts/tts.h: playNumber()/playDuration() dispatch through
// currentLanguagePack). Installing a pack of our own turns every value the
// radio would speak — PLAY_VALUE, timer minutes and countdowns, telemetry —
// into an event carrying the number, its unit and flags, instead of a string
// of prompt files.
//
// A duration is what EdgeTX's English pack makes of it (en_playDuration,
// translations/tts/tts_en.cpp): which numbers, in which units, joined by which
// prompts. While it runs, s_speechParts collects that sequence instead of the
// events and files it would produce, and the duration event carries it as
// "parts": {"n": number, "u": unit} per number, {"p": prompt} per prompt file
// (the prompt's number: 110 "and", 111 "minus" in every pack's numbering). So
// the host only puts words to what EdgeTX decided to say.
static std::string* s_speechParts = nullptr;

static void speechPart(const char* fmt, ...)
{
  char part[64];
  va_list args;
  va_start(args, fmt);
  vsnprintf(part, sizeof(part), fmt, args);
  va_end(args);
  if (!s_speechParts->empty()) s_speechParts->push_back(',');
  *s_speechParts += part;
}

static void etxPlayNumber(getvalue_t number, uint8_t unit, uint8_t flags, uint8_t id, int8_t)
{
  if (s_speechParts) {
    speechPart("{\"n\":%d,\"u\":%u,\"f\":%u}", (int)number, unit, flags);
    return;
  }
  etxEventf("\"ev\":\"number\",\"value\":%d,\"unit\":%u,\"flags\":%u,\"id\":%u", (int)number,
            unit, flags, id);
}

void en_playDuration(int seconds, uint8_t flags, uint8_t id, int8_t fragmentVolume);

static void etxPlayDuration(int seconds, uint8_t flags, uint8_t id, int8_t volume)
{
  std::string parts;
  s_speechParts = &parts;
  en_playDuration(seconds, flags, id, volume);
  s_speechParts = nullptr;
  etxEventf("\"ev\":\"duration\",\"seconds\":%d,\"flags\":%u,\"id\":%u,\"parts\":[%s]",
            seconds, flags, id, parts.c_str());
}

#if defined(ALL_LANGS)
static const char* etxLanguageName() { return "English"; }
static const LanguagePack etxLanguagePack = {"en", etxLanguageName, etxPlayNumber, etxPlayDuration};
#else
static const LanguagePack etxLanguagePack = {"en", "English", etxPlayNumber, etxPlayDuration};
#endif

// Link-time wrappers (wasm-ld --wrap, listed in cmake/inject.cmake). Each
// reports what the radio was asked to play or show, then either lets EdgeTX
// continue (so it maps the event to the tones/pattern it would use) or stops
// there because the next step would need hardware, a screen or the SD card.
// Nothing is ever queued for playback, so EdgeTX sees every sound as finished
// the moment it was requested (IS_PLAYING() is false): a repeating PLAY_*
// special function repeats on its own schedule.
extern "C" {

void __real__Z10audioEventj(unsigned int index);
void __wrap__Z10audioEventj(unsigned int index)
{
  etxEventf("\"ev\":\"sound\",\"id\":%u", index);
  __real__Z10audioEventj(index);
}

void __real__Z9audioPlayjh(unsigned int index, uint8_t id);
void __wrap__Z9audioPlayjh(unsigned int index, uint8_t id)
{
  etxEventf("\"ev\":\"sound\",\"id\":%u,\"prompt\":%u", index, id);
  __real__Z9audioPlayjh(index, id);
}

void __real__Z14audioTrimPressi(int value);
void __wrap__Z14audioTrimPressi(int value)
{
  etxEventf("\"ev\":\"trim\",\"value\":%d", value);
  __real__Z14audioTrimPressi(value);
}

void __real__Z19audioTimerCountdownhi(uint8_t timer, int value);
void __wrap__Z19audioTimerCountdownhi(uint8_t timer, int value)
{
  etxEventf("\"ev\":\"countdown\",\"timer\":%u,\"value\":%d", timer, value);
  __real__Z19audioTimerCountdownhi(timer, value);
}

void __real__Z14playModelEventhht(uint8_t category, uint8_t index, uint16_t event);
void __wrap__Z14playModelEventhht(uint8_t category, uint8_t index, uint16_t event)
{
  etxEventf("\"ev\":\"model_audio\",\"category\":%u,\"index\":%u,\"event\":%u", category, index,
            event);
  __real__Z14playModelEventhht(category, index, event);
}

// AudioQueue::playFile(const char*, uint8_t flags, uint8_t id, int8_t volume)
void __wrap__ZN10AudioQueue8playFileEPKchha(void*, const char* filename, uint8_t flags,
                                           uint8_t id, int8_t)
{
  if (s_speechParts) {
    // A prompt of a duration being put together (pushPrompt(): SYSTEM/NNNN.wav).
    const char* base = filename ? strrchr(filename, '/') : nullptr;
    speechPart("{\"p\":%d}", base ? atoi(base + 1) : -1);
    return;
  }
  etxEventf("\"ev\":\"file\",\"path\":%s,\"flags\":%u,\"id\":%u", jsonQuote(filename).c_str(),
            flags, id);
}

// AudioQueue::playTone(freq, len, pause, flags, freqIncr, volume)
void __wrap__ZN10AudioQueue8playToneEttthaa(void*, uint16_t freq, uint16_t len, uint16_t pause,
                                           uint8_t flags, int8_t freqIncr, int8_t)
{
  etxEventf("\"ev\":\"tone\",\"freq\":%u,\"len\":%u,\"pause\":%u,\"flags\":%u,\"freqIncr\":%d",
            freq, len, pause, flags, freqIncr);
}

#if defined(HAPTIC)
// hapticQueue::event(uint8_t): whether the radio vibrates for an event depends on
// the radio's haptic mode, and in a simulator build EdgeTX only counts it
// (simuHapticValue) instead of queueing a pattern (haptic.cpp). The event is
// reported when EdgeTX counted it, with the id that selects the pattern.
extern uint32_t simuHapticValue;
void __real__ZN11hapticQueue5eventEh(void* self, uint8_t e);
void __wrap__ZN11hapticQueue5eventEh(void* self, uint8_t e)
{
  uint32_t before = simuHapticValue;
  __real__ZN11hapticQueue5eventEh(self, e);
  if (simuHapticValue != before) etxEventf("\"ev\":\"haptic\",\"id\":%u", e);
}

// hapticQueue::play(len, pause, flags, intensity), in 10 ms units: an explicit
// pattern (the power-on buzz and a few others call it directly).
void __wrap__ZN11hapticQueue4playEhhhh(void*, uint8_t len, uint8_t pause, uint8_t flags,
                                       uint8_t intensity)
{
  etxEventf("\"ev\":\"haptic_pattern\",\"len\":%u,\"pause\":%u,\"flags\":%u,"
            "\"intensity\":%u",
            len, pause, flags, intensity);
}
#endif

// checkAll(bool): the pre-flight checks (throttle, switches, failsafe...). On
// the radio these are modal screens that wait for the pilot; here the event is
// reported and nothing waits.
void __wrap__Z8checkAllb(bool isBootCheck)
{
  etxEventf("\"ev\":\"checks\",\"boot\":%s", isBootCheck ? "true" : "false");
}

// Warnings the radio would pop up (for example loadCurves() when the curve
// points do not fit).
void __wrap__Z13POPUP_WARNINGPKcS0_(const char* message, const char* info)
{
  etxEventf("\"ev\":\"warning\",\"message\":%s,\"info\":%s", jsonQuote(message).c_str(),
            jsonQuote(info ? info : "").c_str());
}
bool __wrap__Z24POPUP_WARNING_ON_UI_TASKPKcS0_(const char* message, const char* info)
{
  __wrap__Z13POPUP_WARNINGPKcS0_(message, info);
  return true;
}

#if defined(COLORLCD)
// A new model on a colour radio starts from its screens being torn down
// (applyDefaultTemplate() in model_init.cpp): there are none here.
void __wrap__ZN13LayoutFactory19deleteCustomScreensEv() {}
void __wrap__ZN13LayoutFactory19deleteTopBarWidgetsEv() {}
// The radio YAML writer stores a "current screen" key shortcut as the screen
// the main view shows (RadioData::getKeyShortcut()): the first one.
void* __wrap__ZN8ViewMain8instanceEv() { return nullptr; }
unsigned __wrap__ZNK8ViewMain18getCurrentMainViewEv(const void*) { return 0; }
#endif

// The notification bubble (e.g. "touch disabled", DISABLE_TOUCH function).
void __wrap__Z12POPUP_BUBBLEPKcjii(const char* message, uint32_t timeout, int, int)
{
  etxEventf("\"ev\":\"bubble\",\"message\":%s,\"timeout\":%u", jsonQuote(message).c_str(),
            (unsigned)timeout);
}

// SCREENSHOT and SET_SCREEN special functions (UI functions): there is no
// screen to capture or switch.
void __wrap__Z15writeScreenshotv() { etxEventf("\"ev\":\"screenshot\""); }
void __wrap__Z20setRequestedMainViewh(uint8_t view)
{
  etxEventf("\"ev\":\"screen\",\"value\":%u", view);
}
void __wrap__Z15showTelemScreenh(uint8_t index)
{
  etxEventf("\"ev\":\"screen\",\"value\":%u", index);
}

#if defined(LUA)
// setModelDefaults() runs the SD card's model wizard script on B&W radios
// when there is one. There is no card, and model scripts are not run here.
void __wrap__Z7luaExecPKc(const char* filename)
{
  etxEventf("\"ev\":\"lua\",\"file\":%s", jsonQuote(filename).c_str());
}
#endif

#if !defined(COLORLCD) && defined(GUI)
// On B&W radios special functions test which menu is on screen: INSTANT_TRIM
// works only on the main, telemetry or channel views (IS_INSTANT_TRIM_ALLOWED,
// gui/gui_common.h). Taking those menus' addresses would link the whole menu
// system; the wrappers stand in for them, and etx_init() puts the main view
// on screen, which is where a pilot flying the model is.
void __wrap__Z12menuMainViewt(event_t) {}
void __wrap__Z17menuViewTelemetryt(event_t) {}
void __wrap__Z16menuChannelsViewt(event_t) {}
#endif

}  // extern "C"

// ---- model data fix-ups -------------------------------------------------------

// storage_common.cpp sorts mix lines by destination channel after every load
// (sortMixerLines(), file-static there): bubble passes that stop at the first
// empty line. The same rule, so a hand-written file behaves as on the radio.
static bool sortMixLines()
{
  bool changed = false;
  bool swapped;
  do {
    swapped = false;
    for (int i = 0; i < MAX_MIXERS - 1; i++) {
      MixData* a = mixAddress(i);
      MixData* b = mixAddress(i + 1);
      if (b->destCh < a->destCh) {
        if (is_memclear(b, sizeof(MixData))) break;
        MixData tmp;
        memcpy(&tmp, a, sizeof(MixData));
        memcpy(a, b, sizeof(MixData));
        memcpy(b, &tmp, sizeof(MixData));
        swapped = changed = true;
      }
    }
  } while (swapped);
  return changed;
}

#if defined(PXX2)
// Whether the radio's owner ID is the one this virtual board makes up for a
// radio.yml that has none (setDefaultOwnerId(), from the board's CPU id: the
// same on every install). That ID is nobody's radio, so a model is not given it
// (below, and after setModelDefaults() in newModel()): a model made or imported
// here keeps a blank ID and takes the ID of the radio it is copied to, as a
// model Companion writes does. An owner ID from the pilot's own radio.yml is
// given to models as the radio gives its own.
static bool virtualOwnerId()
{
  char saved[PXX2_LEN_REGISTRATION_ID];
  memcpy(saved, g_eeGeneral.ownerRegistrationID, sizeof(saved));
  setDefaultOwnerId();
  bool same = memcmp(saved, g_eeGeneral.ownerRegistrationID, sizeof(saved)) == 0;
  memcpy(g_eeGeneral.ownerRegistrationID, saved, sizeof(saved));
  return same;
}
#endif

// The data half of postModelLoad() (storage_common.cpp): what the radio
// corrects in a model it has just read, without touching anything that runs.
static void applyModelFixups()
{
#if LCD_W == 128
  g_model.modelGVDisabled = false;
#endif
  if (g_model.noGlobalFunctions) {
    g_model.radioGFDisabled = OVERRIDE_OFF;
    g_model.noGlobalFunctions = 0;
  }
  if (g_model.rssiSource) g_model.rssiSource = 0;
#if defined(PXX2)
  if (is_memclear(g_model.modelRegistrationID, PXX2_LEN_REGISTRATION_ID) &&
      !is_memclear(g_eeGeneral.ownerRegistrationID, PXX2_LEN_REGISTRATION_ID) &&
      !virtualOwnerId())
    memcpy(g_model.modelRegistrationID, g_eeGeneral.ownerRegistrationID,
           PXX2_LEN_REGISTRATION_ID);
#endif
  loadCurves();
  sortMixLines();
  updateMixCount();
}

// ---- runtime reset ------------------------------------------------------------

static uint32_t s_ticks = 0;         // 10 ms ticks since etx_init
static uint32_t s_pendingMs = 0;     // step() remainder below one tick
static int s_lastVolume = -1;
static int s_lastBacklight = -1;
static int s_lastLogs = -1;

// The live model while something borrows g_model (settleFlightModeFade(),
// loadModelYaml()).
static ModelData s_modelBackup;
static bool s_freshStart = false;  // the next tick is the first after a reset

extern tmr10ms_t flightModeTransitionTime;  // mixer.cpp

// evalMixes() keeps the flight-mode fade in function statics (fp_act[], the
// fade mask) that only its own transitions change, and they keep one rule:
// with no fade running, the flight mode the mixer is in weighs MAX_ACT and
// every other weighs 0. Restarting the mixer by setting lastFlightMode back to
// 255, as a boot would, breaks that rule for the flight mode it was in (a boot
// starts from all zeros): the next fade then starts from two full weights, a
// jump to a half-and-half blend, or an int32 overflow in the blend sum on a
// channel above 100 %, which put -256 on an output that should have stayed at
// 1024. So the fade is brought to rest instead, through EdgeTX's own
// transitions: with an empty model standing in (no fade times, no mixes, a
// flight-mode switch that is simply ON), evalMixes() is walked through every
// flight mode, ending on the one the mixer was in. Each of those transitions is
// instant, zeroes the mode it leaves and clears both modes from the fade mask.
// (Where the model's switches point then is firstMixerRunAfterReset()'s
// business.)
static void settleFlightModeFade()
{
  if (lastFlightMode >= MAX_FLIGHT_MODES) return;  // never mixed: the boot state
  uint8_t rest = lastFlightMode;
  memcpy(&s_modelBackup, &g_model, sizeof(g_model));
  memclear(&g_model, sizeof(g_model));
  flightModeTransitionTime = 0;  // no announcement of these transitions
  for (int step = 1; step <= MAX_FLIGHT_MODES; step++) {
    uint8_t fm = (rest + step) % MAX_FLIGHT_MODES;
    if (fm) g_model.flightModeData[fm].swtch = SWSRC_ON;
    evalMixes(0);
    if (fm) g_model.flightModeData[fm].swtch = SWSRC_NONE;
  }
  memcpy(&g_model, &s_modelBackup, sizeof(g_model));
  flightModeTransitionTime = 0;
}

// What the radio resets when it switches to a model (the runtime half of
// postModelLoad(), storage_common.cpp), plus the mixer state EdgeTX's own unit
// tests clear between models (tests/gtests.h MODEL_RESET/MIXER_RESET) and no
// flight-mode fade in progress.
static void resetRuntime()
{
  // First: its evaluations leave outputs, mixer state and logical-switch copies
  // behind, all of which are reset below.
  settleFlightModeFade();
  flightReset(false);
  customFunctionsReset();
  logicalSwitchesInit(false);
  restoreTimers();
#if defined(FUNCTION_SWITCHES)
  setFSStartupPosition();
#endif
  for (int i = 0; i < MAX_TELEMETRY_SENSORS; i++) {
    TelemetrySensor& sensor = g_model.telemetrySensors[i];
    if (sensor.type == TELEM_TYPE_CALCULATED && sensor.persistent) {
      telemetryItems[i].value = sensor.persistentValue;
      telemetryItems[i].timeout = 0;
    } else {
      telemetryItems[i].timeout = TELEMETRY_SENSOR_TIMEOUT_UNAVAILABLE;
    }
  }

  memclear(channelOutputs, sizeof(channelOutputs));
  memclear(chans, sizeof(chans));
  memclear(ex_chans, sizeof(ex_chans));
  memclear(act, sizeof(act));
  memclear(mixState, sizeof(mixState));
#if defined(OVERRIDE_CHANNEL_FUNCTION)
  for (int i = 0; i < MAX_OUTPUT_CHANNELS; i++) safetyCh[i] = OVERRIDE_CHANNEL_UNDEFINED;
#endif
  s_mixer_first_run_done = false;
  s_freshStart = true;
}

// The first mixer run after a reset. The fade rests in the flight mode the
// mixer was in (settleFlightModeFade()); the model's switches may select
// another. Like a power-on (lastFlightMode 255 in evalMixes()) the mixer starts
// in the selected flight mode without fading from one the model was not
// running, and announces it; the model's fade times apply from the next
// change on.
static void firstMixerRunAfterReset()
{
  uint8_t fadeIn[MAX_FLIGHT_MODES], fadeOut[MAX_FLIGHT_MODES];
  for (int i = 0; i < MAX_FLIGHT_MODES; i++) {
    fadeIn[i] = g_model.flightModeData[i].fadeIn;
    fadeOut[i] = g_model.flightModeData[i].fadeOut;
    g_model.flightModeData[i].fadeIn = g_model.flightModeData[i].fadeOut = 0;
  }
  flightModeTransitionTime = get_tmr10ms();
  flightModeTransitionLast = 255;
  doMixerCalculations();
  for (int i = 0; i < MAX_FLIGHT_MODES; i++) {
    g_model.flightModeData[i].fadeIn = fadeIn[i];
    g_model.flightModeData[i].fadeOut = fadeOut[i];
  }
}

// ---- the tick -------------------------------------------------------------------

// s_anaFilt holds the ADC jitter filter state (hal/adc_driver.cpp). The
// virtual radio's inputs carry no ADC noise and are meant to be exact, so the
// state is primed before each read to a value no input can be within the
// filter's window of: getADC() then takes every value unfiltered, and applies
// the rest of its pipeline (inversion, multipos quantisation) as it would.
extern uint32_t s_anaFilt[MAX_ANALOG_INPUTS];

static void primeAnalogFilter()
{
  for (int i = 0; i < MAX_ANALOG_INPUTS; i++) s_anaFilt[i] = 0xFFFF0u;
}

static void reportEffects()
{
  int volume = requiredSpeakerVolume;
  if (volume != s_lastVolume) {
    if (s_lastVolume >= 0) etxEventf("\"ev\":\"volume\",\"value\":%d", volume);
    s_lastVolume = volume;
  }
  int backlight = requiredBacklightBright;
  if (backlight != s_lastBacklight) {
    if (s_lastBacklight >= 0) etxEventf("\"ev\":\"backlight\",\"value\":%d", backlight);
    s_lastBacklight = backlight;
  }
  int logs = isFunctionActive(FUNCTION_LOGS) ? logDelay100ms : 0;
  if (logs != s_lastLogs) {
    if (s_lastLogs >= 0)
      etxEventf("\"ev\":\"logs\",\"on\":%s,\"period100ms\":%d", logs ? "true" : "false", logs);
    s_lastLogs = logs;
  }
}

// One 10 ms tick of the radio, in the order the radio runs it:
//   - the 10 ms timer interrupt: per10ms() (g_tmr10ms, key and trim button
//     polling and repeat, function switches, telemetry timers);
//   - the mixer task: doMixerCalculations() (ADC, switch positions, inputs,
//     flight mode, logical switches, mixes, special functions, limits) and
//     doMixerPeriodicUpdates() (timers, logical switch delays/durations, trim
//     button handling);
//   - every fifth tick, the part of the UI task's perMain() that mixing and
//     effects depend on: evalUIFunctions() for volume, backlight, logging and
//     flight reset.
// Pulses are not generated: channelOutputs is what they would be made from.
static void runTick()
{
  etxAdvanceVirtualMs(10);
  per10ms();
  primeAnalogFilter();
  if (s_freshStart) {
    s_freshStart = false;
    firstMixerRunAfterReset();
  } else {
    doMixerCalculations();
  }
  doMixerPeriodicUpdates();
  s_ticks++;
  if (s_ticks % 5 == 0) {
    if (radioGFEnabled()) evalUIFunctions(g_eeGeneral.customFn, globalFunctionsContext);
    if (modelSFEnabled()) evalUIFunctions(g_model.customFn, modelFunctionsContext);
  }
  reportEffects();
}

// ---- YAML ---------------------------------------------------------------------

static std::string modelPath() { return std::string(MODELS_PATH "/") + ETX_MODEL_FILE; }

// Parse into scratch space first, with EdgeTX's own parser and schema, so a
// file that is not a model (or overflows the parser) leaves g_model alone.
static ModelData s_scratch;

static int32_t checkModelYaml(const char* data, int32_t len)
{
  memset(&s_scratch, 0, sizeof(s_scratch));
  YamlTreeWalker tree;
  tree.reset(get_modeldata_nodes(), (uint8_t*)&s_scratch);
  YamlParser parser;
  parser.init(YamlTreeWalker::get_parser_calls(), &tree);
  parser.set_eof();
  if (parser.parse(data, len) == YamlParser::STRING_OVERFLOW)
    return fail(ETX_ERR_OVERFLOW, "a value is longer than EdgeTX's YAML parser accepts");
  if (is_memclear(&s_scratch, sizeof(s_scratch)))
    return fail(ETX_ERR_NOT_A_MODEL, "no model data recognised");
  return ETX_OK;
}

// The live model as EdgeTX's writer produces it (etx_write_model_yaml), or
// nullptr with the writer's error.
static const char* liveModelYaml(std::string& text)
{
  std::string path = modelPath();
  const char* error = writeModelYaml(ETX_MODEL_FILE);
  const uint8_t* data;
  uint32_t len;
  text.clear();
  if (!error && etxFsGet(path.c_str(), &data, &len)) text.assign((const char*)data, len);
  etxFsRemove(path.c_str());
  return error;
}

static int32_t loadModelYaml(const char* data, int32_t len)
{
  if (!data || len <= 0) return fail(ETX_ERR_EMPTY, "empty model");
  // A few of EdgeTX's model readers do not write into the structure they are
  // handed but straight into model storage: the colour radios' screens and top
  // bar and the Lua user data (statics beside g_model), and on function-switch
  // radios the pre-3.0 switch keys and names (into g_model itself). So the
  // check parse into scratch space still reaches the live model; a file that is
  // refused must leave it as it was, screens and user data included. The live
  // model is kept as EdgeTX writes it (for the storage beside g_model, which
  // readModel() resets and re-reads) and as bytes (for g_model, exactly).
  std::string live;
  liveModelYaml(live);
  memcpy(&s_modelBackup, &g_model, sizeof(g_model));
  uint8_t dirty = storageDirtyMsk;
  int32_t rc = checkModelYaml(data, len);
  if (rc != ETX_OK) {
    std::string path = modelPath();
    etxFsPut(path.c_str(), (const uint8_t*)live.data(), (uint32_t)live.size());
    readModel(ETX_MODEL_FILE, (uint8_t*)&g_model, sizeof(g_model), MODELS_PATH);
    etxFsRemove(path.c_str());
    memcpy(&g_model, &s_modelBackup, sizeof(g_model));
    storageDirtyMsk = dirty;
    return rc;
  }

  std::string path = modelPath();
  etxFsPut(path.c_str(), (const uint8_t*)data, (uint32_t)len);
  // EdgeTX's reader: wipes the model, applies the pre-load defaults
  // (function switches, GV links, RF alarms) and parses the file.
  const char* error = readModel(ETX_MODEL_FILE, (uint8_t*)&g_model, sizeof(g_model), MODELS_PATH);
  etxFsRemove(path.c_str());
  if (error) {
    // Same recovery as loadModel(): a clean default model the mixer can run.
    setModelDefaults();
    applyModelFixups();
    return fail(ETX_ERR_STORAGE, error);
  }
  applyModelFixups();
  storageDirtyMsk &= ~EE_MODEL;
  s_lastError.clear();
  return ETX_OK;
}

// ---- exports: lifecycle ---------------------------------------------------------

ETX_EXPORT(etx_api_version) int32_t etx_api_version() { return ETX_API_VERSION; }

static void newModel(int32_t channelOrder)
{
  uint8_t saved = g_eeGeneral.templateSetup;
  if (channelOrder >= 0 && channelOrder < (int32_t)inputMappingGetMaxChannelOrder())
    g_eeGeneral.templateSetup = channelOrder;
  setModelDefaults();
  g_eeGeneral.templateSetup = saved;
#if defined(PXX2)
  if (virtualOwnerId()) memclear(g_model.modelRegistrationID, PXX2_LEN_REGISTRATION_ID);
#endif
  applyModelFixups();
  resetRuntime();
  storageDirtyMsk &= ~EE_MODEL;
}

ETX_EXPORT(etx_init) int32_t etx_init()
{
  etxSetVirtualMs(0);
  g_tmr10ms = 0;
  s_ticks = 0;
  s_pendingMs = 0;
  s_events.clear();
  s_lastError.clear();

  etxBoardInit();

  // Radio defaults for this build, as on a radio with no radio.yml
  // (storageReadAll() + storageFormat()), then Mode 2.
  memset(&g_eeGeneral, 0, sizeof(g_eeGeneral));
  generalDefault();
#if defined(ETX_VIRTUAL_RADIO)
  // generalDefault() calibrates the base radio's 6-position pot; here that slot
  // belongs to no input, and EdgeTX's writer would give the entry no name.
  for (int i = adcGetMaxCalibratedInputs(); i < (int)DIM(g_eeGeneral.calib); i++)
    memset(&g_eeGeneral.calib[i], 0, sizeof(g_eeGeneral.calib[i]));
#endif
  g_eeGeneral.stickMode = 1;
  g_eeGeneral.chkSum = evalChkSum();
  postRadioSettingsLoad();
  currentLanguagePack = &etxLanguagePack;

#if !defined(COLORLCD) && defined(GUI)
  menuLevel = 0;
  menuHandlers[0] = __wrap__Z12menuMainViewt;
#endif

  newModel(-1);
  storageDirtyMsk = 0;

  s_lastVolume = requiredSpeakerVolume =
      limit<int>(0, g_eeGeneral.speakerVolume + VOLUME_LEVEL_DEF, VOLUME_LEVEL_MAX);
  s_lastBacklight = requiredBacklightBright = g_eeGeneral.getBrightness();
  s_lastLogs = 0;
  return ETX_OK;
}

ETX_EXPORT(etx_last_error) int32_t etx_last_error(char* out, int32_t max)
{
  return etx::copyOut(s_lastError, out, max);
}

ETX_EXPORT(etx_set_stick_mode) int32_t etx_set_stick_mode(int32_t mode)
{
  int32_t previous = g_eeGeneral.stickMode;
#if !defined(SURFACE_RADIO)
  if (mode >= 0 && mode <= 3) g_eeGeneral.stickMode = mode;
#endif
  return previous;
}

ETX_EXPORT(etx_load_radio_yaml) int32_t etx_load_radio_yaml(const char* data, int32_t len)
{
  if (!data || len <= 0) return fail(ETX_ERR_EMPTY, "empty radio settings");
  etxFsPut(RADIO_SETTINGS_YAML_PATH, (const uint8_t*)data, (uint32_t)len);
  // loadRadioSettings() (sdcard_yaml.cpp) without the checksum recovery,
  // which renames files and raises alerts: a file edited off the radio is
  // simply taken as it is (the radio does the same when manuallyEdited is set).
  memset(&g_eeGeneral, 0, sizeof(g_eeGeneral));
  // The pot each flex switch reads is not in g_eeGeneral: switch_driver.cpp keeps it in a
  // static (_flex_switches) that the reader sets only where the file has flexSwitches. The
  // radio reads its settings once, after switchInit() cleared that static; clear it the same
  // way before every read, or a file without flexSwitches keeps the pot the last one named.
  for (int i = 0; i < (int)MAX_FLEX_SWITCHES; i++) switchConfigFlex_raw((uint8_t)i, -1);
  g_eeGeneral.modelCustomScriptsDisabled = true;
#if defined(DEFAULT_INTERNAL_MODULE)
  g_eeGeneral.internalModule = DEFAULT_INTERNAL_MODULE;
#endif
  adcCalibDefaults();
  generalDefaultSwitches();
#if defined(COLORLCD)
  g_eeGeneral.defaultKeyShortcuts();
#endif
  // attemptLoad() of sdcard_yaml.cpp: EdgeTX's radio schema, EdgeTX's reader.
  YamlTreeWalker tree;
  tree.reset(get_radiodata_nodes(), (uint8_t*)&g_eeGeneral);
  const char* error =
      readYamlFile(RADIO_SETTINGS_YAML_PATH, YamlTreeWalker::get_parser_calls(), &tree, nullptr);
  etxFsRemove(RADIO_SETTINGS_YAML_PATH);
  g_eeGeneral.chkSum = evalChkSum();
  postRadioSettingsLoad();
  if (g_eeGeneral.uiLanguage[0] == 0) generalDefaultUILanguage();
  storageDirtyMsk &= ~EE_GENERAL;
  if (error) return fail(ETX_ERR_STORAGE, error);
  s_lastError.clear();
  return ETX_OK;
}

ETX_EXPORT(etx_write_radio_yaml) int32_t etx_write_radio_yaml(char* out, int32_t max)
{
  const char* error = writeGeneralSettings();
  if (error) {
    fail(ETX_ERR_STORAGE, error);
    return -ETX_ERR_STORAGE;
  }
  const uint8_t* data;
  uint32_t len;
  std::string text;
  if (etxFsGet(RADIO_SETTINGS_YAML_PATH, &data, &len)) text.assign((const char*)data, len);
  etxFsRemove(RADIO_SETTINGS_YAML_PATH);
  return etx::copyOut(text, out, max);
}

ETX_EXPORT(etx_load_model_yaml) int32_t etx_load_model_yaml(const char* data, int32_t len)
{
  return loadModelYaml(data, len);
}

ETX_EXPORT(etx_new_model) int32_t etx_new_model(int32_t channelOrder)
{
  newModel(channelOrder);
  return ETX_OK;
}

ETX_EXPORT(etx_write_model_yaml) int32_t etx_write_model_yaml(char* out, int32_t max)
{
  std::string text;
  const char* error = liveModelYaml(text);
  if (error) {
    fail(ETX_ERR_STORAGE, error);
    return -ETX_ERR_STORAGE;
  }
  return etx::copyOut(text, out, max);
}

ETX_EXPORT(etx_reset_runtime) void etx_reset_runtime() { resetRuntime(); }

// What the radio stores into the model before it leaves it (model switch,
// power off): persistent timers, persistent calculated sensors, pot positions.
ETX_EXPORT(etx_flush_model) void etx_flush_model() { storageFlushCurrentModel(); }

// The radio's "Reset flight" (its quick menu, a RESET special function):
// flightReset() (edgetx.cpp), which starts over the timers that are not
// manual-reset, telemetry, logical switches and the throttle trace, then runs
// the pre-flight checks (reported as a "checks" event). Not a model switch:
// persistent timers keep what they count; etx_reset_runtime() is the switch.
// *added*
ETX_EXPORT(etx_flight_reset) void etx_flight_reset() { flightReset(true); }

// What the radio writes into a model's persistent timers before it leaves the
// model (saveTimers(), timers.cpp; part of storageFlushCurrentModel(), without
// the pot positions that also stores). Marks the model changed when a value
// moved. *added*
ETX_EXPORT(etx_save_timers) void etx_save_timers() { saveTimers(); }

// Say `seconds` the way the radio announces a duration (playDuration(), as a
// PLAY_VALUE of a timer or a timer's minute call does): a "duration" event with
// its parts. `flags`: 1 a time of day, 2 the long-timer form (audio.h). *added*
ETX_EXPORT(etx_play_duration) void etx_play_duration(int32_t seconds, int32_t flags)
{
  playDuration(seconds, (uint8_t)flags, 0);
}

ETX_EXPORT(etx_model_changed) int32_t etx_model_changed()
{
  bool dirty = storageDirtyMsk & EE_MODEL;
  storageDirtyMsk &= ~EE_MODEL;
  return dirty ? 1 : 0;
}

// ---- exports: inputs ------------------------------------------------------------

// value: the calibrated position of analog input `index` (hardware order:
// main sticks, then flex inputs), -1024..1024. The raw value handed to
// EdgeTX's ADC layer is 2 * (value + 1024) on its 0..4096 simulator scale,
// which its pipeline (no calibration in simulator builds, radio inversion
// settings, jitter filter bypassed, multipos quantisation) turns back into
// exactly `value` in calibratedAnalogs[] for a non-inverted input.
ETX_EXPORT(etx_set_analog) int32_t etx_set_analog(int32_t index, int32_t value)
{
  if (index < 0 || index >= adcGetMaxInputs(ADC_INPUT_MAIN) + adcGetMaxInputs(ADC_INPUT_FLEX))
    return -ETX_ERR_ARGUMENT;
  if (value < -RESX) value = -RESX;
  if (value > RESX) value = RESX;
  etxBoardSetAnalog(index, (uint16_t)(2 * (value + RESX)));
  return ETX_OK;
}

// position: -1 up, 0 middle, +1 down (EdgeTX's simuSetSwitch convention).
// A function switch (CFS) button is pressed while its position is not up.
ETX_EXPORT(etx_set_switch) int32_t etx_set_switch(int32_t index, int32_t position)
{
  if (index < 0 || index >= switchGetMaxSwitches()) return -ETX_ERR_ARGUMENT;
  etxBoardSetSwitch(index, (int8_t)(position < 0 ? -1 : position > 0 ? 1 : 0));
  return ETX_OK;
}

// trim: hardware trim index (T1 = left horizontal, T2 = left vertical, T3 =
// right vertical, T4 = right horizontal, T5.. extra), as the trim buttons are
// wired; dir: -1 hold the decrement button, +1 hold the increment button, 0
// release. EdgeTX's key debounce, repeat and trim handling do the rest, in
// the flight mode and with the rules of the loaded model.
ETX_EXPORT(etx_set_trim_key) int32_t etx_set_trim_key(int32_t trim, int32_t dir)
{
  if (trim < 0 || trim >= keysGetMaxTrims()) return -ETX_ERR_ARGUMENT;
  etxBoardSetTrimKey(2 * trim, dir < 0);
  etxBoardSetTrimKey(2 * trim + 1, dir > 0);
  return ETX_OK;
}

ETX_EXPORT(etx_set_key) int32_t etx_set_key(int32_t key, int32_t pressed)
{
  if (key < 0 || key >= MAX_KEYS) return -ETX_ERR_ARGUMENT;
  etxBoardSetKey(key, pressed != 0);
  return ETX_OK;
}

// Host inputs (radio/src/hal/host_inputs.h; the virtual radio only): the
// controls of whatever the host can read, each a source of the mixer in its
// own right. `index` is describe().hostAxes[].index / hostButtons[].index.
//
// An axis takes its position -1024..1024, as the host reports it: no
// calibration, no inversion, no dead zone -- a model line's weight and curve
// are where a pilot shapes it. A button takes how far it is pressed, -1024
// released .. 1024 pressed, and is on (as a switch) above zero; what a press
// means -- held, latched, one of a group -- is the host's decision, made
// before it gets here. Both return -ETX_ERR_ARGUMENT for an index this radio
// lacks, which is every index on a radio without host inputs. *added*
ETX_EXPORT(etx_set_host_axis) int32_t etx_set_host_axis(int32_t index, int32_t value)
{
#if defined(HOST_INPUTS)
  if (etxHostSetAxis(index, value)) return ETX_OK;
#else
  (void)index;
  (void)value;
#endif
  return -ETX_ERR_ARGUMENT;
}

ETX_EXPORT(etx_set_host_button) int32_t etx_set_host_button(int32_t index, int32_t value)
{
#if defined(HOST_INPUTS)
  if (etxHostSetButton(index, value)) return ETX_OK;
#else
  (void)index;
  (void)value;
#endif
  return -ETX_ERR_ARGUMENT;
}

// ---- exports: time --------------------------------------------------------------

// Advance the radio by `ms` milliseconds in 10 ms ticks; a remainder is kept
// for the next call. Returns the number of ticks run.
//
// The outputs are fresh on return whether or not a tick ran: a call too short
// to reach the next tick still runs the mixer over the inputs as they are now,
// with no time passed. That is what a radio does - its mixer task runs at the
// RF module's rate, up to 1 kHz, between two 10 ms ticks of its clock
// (doMixerCalculations() hands evalMixes() the ticks since its last run, zero
// included) - and it is what lets the host read channels more often than every
// 10 ms without getting the previous inputs' answer.
ETX_EXPORT(etx_step) int32_t etx_step(int32_t ms)
{
  if (ms <= 0) return 0;
  s_pendingMs += (uint32_t)ms;
  int32_t ticks = 0;
  while (s_pendingMs >= 10) {
    s_pendingMs -= 10;
    runTick();
    ticks++;
  }
  // Not before the first tick after a reset: that run starts the flight mode
  // and the switches over (firstMixerRunAfterReset()).
  if (ticks == 0 && !s_freshStart) {
    primeAnalogFilter();
    doMixerCalculations();
  }
  return ticks;
}

ETX_EXPORT(etx_get_time) uint32_t etx_get_time() { return g_tmr10ms; }

// ---- exports: outputs -------------------------------------------------------------

// How many elements an array getter writes: at most `max` (a negative one is
// none, not a huge memcpy) and at most what there is.
static int32_t arrayCount(int32_t max, int32_t capacity)
{
  return max <= 0 ? 0 : max < capacity ? max : capacity;
}

ETX_EXPORT(etx_get_channels) int32_t etx_get_channels(int16_t* out, int32_t max)
{
  int32_t n = arrayCount(max, MAX_OUTPUT_CHANNELS);
  if (out) memcpy(out, channelOutputs, n * sizeof(int16_t));
  return n;
}

// The PPM pulse width the radio would emit for each channel, in half
// microseconds (EdgeTX times pulses with a 2 MHz clock): the channel output
// limited to the PPM range, plus twice the channel's PPM centre
// (pulses/ppm.cpp setupPulsesPPM()). Divide by two for microseconds.
ETX_EXPORT(etx_get_pulses_us) int32_t etx_get_pulses_us(uint16_t* out, int32_t max)
{
  int32_t n = arrayCount(max, MAX_OUTPUT_CHANNELS);
  int16_t range = g_model.extendedLimits ? (512 * LIMIT_EXT_PERCENT / 100) * 2 : 512 * 2;
  for (int32_t i = 0; out && i < n; i++)
    out[i] = (uint16_t)(limit<int16_t>(-range, channelOutputs[i], range) + 2 * PPM_CH_CENTER(i));
  return n;
}

// Mixer outputs before limits (ex_chans).
ETX_EXPORT(etx_get_mixer_outputs) int32_t etx_get_mixer_outputs(int16_t* out, int32_t max)
{
  int32_t n = arrayCount(max, MAX_OUTPUT_CHANNELS);
  if (out) memcpy(out, ex_chans, n * sizeof(int16_t));
  return n;
}

// Bit n set: channel n+1 has at least one mix line the mixer runs. Counted as
// evalFlightModeMixes() walks the lines, not with isChannelUsed(): that one
// stops at the first line without a source on every radio, while the mixer
// stops there only on a B&W radio and skips the line on a colour one -- so a
// colour model with an empty line would have its later channels mixed and
// reported unused.
ETX_EXPORT(etx_get_used_channels) uint32_t etx_get_used_channels()
{
  uint32_t mask = 0;
  for (int i = 0; i < MAX_MIXERS; i++) {
    const MixData* md = mixAddress(i);
    if (md->srcRaw == 0) {
#if defined(COLORLCD)
      continue;
#else
      break;
#endif
    }
    if (md->destCh < MAX_OUTPUT_CHANNELS && md->destCh < 32) mask |= 1u << md->destCh;
  }
  return mask;
}

ETX_EXPORT(etx_get_logical_switches) int32_t etx_get_logical_switches(uint8_t* out, int32_t max)
{
  int32_t n = arrayCount(max, MAX_LOGICAL_SWITCHES);
  for (int32_t i = 0; out && i < n; i++)
    out[i] = getSwitch(SWSRC_FIRST_LOGICAL_SWITCH + i) ? 1 : 0;
  return n;
}

ETX_EXPORT(etx_get_flight_mode) int32_t etx_get_flight_mode() { return getFlightMode(); }

// Effective value of a global variable in flight mode `fm` (following the
// flight mode's links), or in the current flight mode when fm < 0.
ETX_EXPORT(etx_get_gvar) int32_t etx_get_gvar(int32_t gv, int32_t fm)
{
#if defined(GVARS)
  if (gv < 0 || gv >= MAX_GVARS) return 0;
  if (fm < 0 || fm >= MAX_FLIGHT_MODES) fm = getFlightMode();
  return getGVarValue(gv, fm);
#else
  return 0;
#endif
}

// Timer `i`: returns its value in seconds; out (when given) receives
// [value, state, counter-in-10ms] with state TMR_OFF 0, TMR_RUNNING 1,
// TMR_NEGATIVE 2, TMR_STOPPED 3.
ETX_EXPORT(etx_get_timer) int32_t etx_get_timer(int32_t i, int32_t* out)
{
  if (i < 0 || i >= MAX_TIMERS) return 0;
  const TimerState& t = timersStates[i];
  if (out) {
    out[0] = t.val;
    out[1] = t.state;
    out[2] = t.val_10ms;
  }
  return t.val;
}

// Trim `i` in the model's own order (Rud, Ele, Thr, Ail, T5, T6...), as it
// applies in the current flight mode. Every trim the model stores, including
// those this radio has no buttons for (GX12: T5, T6), which still feed their
// mixer sources.
ETX_EXPORT(etx_get_trim) int32_t etx_get_trim(int32_t i)
{
  if (i < 0 || i >= MAX_TRIMS) return 0;
  return getTrimValue(getTrimFlightMode(getFlightMode(), i), i);
}

// Per special function: bit 0 the model's function i is active, bit 1 the
// radio's (global) function i is active.
ETX_EXPORT(etx_get_active_functions) int32_t etx_get_active_functions(uint8_t* out, int32_t max)
{
  int32_t n = arrayCount(max, MAX_SPECIAL_FUNCTIONS);
  for (int32_t i = 0; out && i < n; i++)
    out[i] = (modelFunctionsContext.isFunctionSwitchActive(i) ? 1 : 0) |
             (globalFunctionsContext.isFunctionSwitchActive(i) ? 2 : 0);
  return n;
}

// Bitmask of the special-function effects in force (FUNCTION_TRAINER_STICK1..,
// FUNCTION_INSTANT_TRIM.., FUNCTION_LOGS, FUNCTION_BACKGND_MUSIC..; dataconstants.h).
ETX_EXPORT(etx_get_function_flags) uint32_t etx_get_function_flags()
{
  return globalFunctionsContext.activeFunctions | globalFunctionsContext.activeUIFunctions |
         modelFunctionsContext.activeFunctions | modelFunctionsContext.activeUIFunctions;
}

// Override values from OVERRIDE_CHANNEL special functions (percent), or
// -32768 where none applies.
ETX_EXPORT(etx_get_overrides) int32_t etx_get_overrides(int16_t* out, int32_t max)
{
  int32_t n = arrayCount(max, MAX_OUTPUT_CHANNELS);
#if defined(OVERRIDE_CHANNEL_FUNCTION)
  for (int32_t i = 0; out && i < n; i++)
    out[i] = safetyCh[i] == OVERRIDE_CHANNEL_UNDEFINED ? -32768 : safetyCh[i];
#else
  for (int32_t i = 0; out && i < n; i++) out[i] = -32768;
#endif
  return n;
}

// The calibrated analog values the mixer used in the last tick (after
// inversion, dead zone, throttle reversal and trainer), hardware order.
ETX_EXPORT(etx_get_analogs) int32_t etx_get_analogs(int16_t* out, int32_t max)
{
  int32_t count = adcGetMaxInputs(ADC_INPUT_MAIN) + adcGetMaxInputs(ADC_INPUT_FLEX);
  int32_t n = arrayCount(max, count);
  if (out) memcpy(out, calibratedAnalogs, n * sizeof(int16_t));
  return n;
}

// ---- exports: live highlighting -------------------------------------------------

// What the radio's Inputs and Mixes lists light up (isExpoActive()/isMixActive()
// in edgetx.h, from the last normal mixer run): MAX_EXPOS bytes, one per input
// line (expoData index), 1 when the line is the one currently driving its input
// (mixState[].activeExpo: the first line of the input whose switch, flight
// modes and side are on); then MAX_MIXERS bytes, one per mix line (mixData
// index), 1 when the line contributed this cycle (mixState[].activeMix; a later
// REPLACE line un-marks the earlier lines of its channel). Returns the bytes
// written, at most `max`.
//
// Lines the mixer never reaches read 0 rather than what the flags last held:
// applyExpos() stops at the first line without a mode and leaves the flags of
// the lines after it as they were, and on B&W radios evalMixes() stops at the
// first mix line without a source, leaving the rest of its flag array (a local
// one) uninitialised when it copies it into mixState.
ETX_EXPORT(etx_get_active_lines) int32_t etx_get_active_lines(uint8_t* out, int32_t max)
{
  int32_t n = arrayCount(max, MAX_EXPOS + MAX_MIXERS);
  bool reached = true;
  for (int32_t i = 0; out && i < n && i < MAX_EXPOS; i++) {
    if (!EXPO_VALID(expoAddress(i))) reached = false;
    out[i] = reached && isExpoActive(i) ? 1 : 0;
  }
  reached = true;
  for (int32_t i = MAX_EXPOS; out && i < n; i++) {
    uint8_t mix = i - MAX_EXPOS;
#if !defined(COLORLCD)
    if (mixAddress(mix)->srcRaw == 0) reached = false;
#endif
    out[i] = reached && isMixActive(mix) ? 1 : 0;
  }
  return n;
}

// EdgeTX's current value of mixer source `source` (getValue(): -1024..1024 for
// sticks, pots, inputs, switches, channels..., the raw value for GVs, timers and
// telemetry; negative = inverted), for a curve's crosshair or a logical
// switch's operands. 0 outside the source range.
ETX_EXPORT(etx_get_source_value) int32_t etx_get_source_value(int32_t source)
{
  if (source < -MIXSRC_LAST_TELEM || source > MIXSRC_LAST_TELEM) return 0;
  return getValue((mixsrc_t)source);
}

// Per logical switch: 1 while a Sticky one is latched (getLSStickyState(), the
// radio's bold function label), 0 for every other function.
ETX_EXPORT(etx_get_ls_sticky) int32_t etx_get_ls_sticky(uint8_t* out, int32_t max)
{
  int32_t n = arrayCount(max, MAX_LOGICAL_SWITCHES);
  for (int32_t i = 0; out && i < n; i++)
    out[i] = lswFamily(lswAddress(i)->func) == LS_FAMILY_STICKY && getLSStickyState(i) ? 1 : 0;
  return n;
}

// ---- exports: pre-flight checks (added) ------------------------------------------

bool isThrottleWarningAlertNeeded();  // edgetx.cpp

// Whether the radio would stop at its throttle warning with the inputs as they
// are now (isThrottleWarningAlertNeeded(), edgetx.cpp): false when the model
// turns the warning off, measured against the custom idle position when it
// sets one, on the throttle source the model traces. Set the inputs first.
// *added*
ETX_EXPORT(etx_check_throttle) int32_t etx_check_throttle()
{
  return isThrottleWarningAlertNeeded() ? 1 : 0;
}

// Whether the radio would stop at its switch warning with the inputs as they
// are now (isSwitchWarningRequired(), switches.cpp). `out`, when given,
// receives one byte per switch in hardware order (describe().switches): 0 where
// the switch is where the model wants it or has no warning, else the position
// the model wants (1 up, 2 middle, 3 down, the switchWarning states), then one
// byte per flex input: 1 for a pot the model warns about that is off its
// stored position. Returns 1 when the radio would warn. *added*
ETX_EXPORT(etx_check_switches) int32_t etx_check_switches(uint8_t* out, int32_t max)
{
  uint16_t badPots = 0;
  bool warn = isSwitchWarningRequired(badPots);
  int32_t switches = switchGetMaxAllSwitches();
  int32_t pots = adcGetMaxInputs(ADC_INPUT_FLEX);
  for (int32_t i = 0; out && i < max && i < switches; i++) {
    uint8_t want = SWITCH_WARNING_ALLOWED(i) ? g_model.getSwitchWarning(i) : 0;
    out[i] = want && want != g_model.getSwitchStateForWarning(i) ? want : 0;
  }
  for (int32_t i = 0; out && switches + i < max && i < pots; i++)
    out[switches + i] = g_model.potsWarnMode ? (badPots >> i) & 1 : 0;
  return warn ? 1 : 0;
}

// ---- exports: curve maths (added) ------------------------------------------------

// EdgeTX's custom-curve maths (applyCustomCurve(), curves.cpp: intpol, or
// hermite_spline when smooth) for a curve given by its points rather than
// stored in the model: the editor's curve before it is committed, or a type or
// point-count change being worked out. `points` as the model stores a curve:
// `count` Y values in percent, then for a custom curve its count - 2 inner X
// values. ys[i] = the curve at xs[i]; `flags` bit 0: xs are percent
// (calc100toRESX), bit 1: ys are returned in percent (calcRESXto100), bit 2:
// the mirrored curve (x negated, a "-CVn" reference); otherwise -1024..1024.
// The model's first curve slot is borrowed for the call and restored. Returns
// the number of values written, 0 when the curve is not one EdgeTX stores.
// *added*
ETX_EXPORT(etx_eval_curve)
int32_t etx_eval_curve(const int8_t* points, int32_t count, int32_t custom, int32_t smooth,
                       const int16_t* xs, int16_t* ys, int32_t n, int32_t flags)
{
  if (!points || !xs || !ys || n <= 0) return 0;
  if (count < 2 || count > MAX_POINTS_PER_CURVE) return 0;
  int32_t stored = custom ? 2 * count - 2 : count;
  if (stored > MAX_CURVE_POINTS) return 0;
  CurveHeader header = g_model.curves[0];
  int8_t saved[MAX_CURVE_POINTS];
  memcpy(saved, g_model.points, stored);
  memcpy(g_model.points, points, stored);
  g_model.curves[0].type = custom ? CURVE_TYPE_CUSTOM : CURVE_TYPE_STANDARD;
  g_model.curves[0].smooth = smooth ? 1 : 0;
  g_model.curves[0].points = count - 5;
  for (int32_t i = 0; i < n; i++) {
    int x = (flags & 1) ? calc100toRESX(xs[i]) : xs[i];
    if (flags & 4) x = -x;
    int y = applyCustomCurve(x, 0);
    ys[i] = (int16_t)((flags & 2) ? calcRESXto100(y) : y);
  }
  memcpy(g_model.points, saved, stored);
  g_model.curves[0] = header;
  return n;
}

// EdgeTX's curve for a Diff, Expo or Function reference with a constant value
// (applyCurve(), curves.cpp): type 0 Diff, 1 Expo, 2 Function (CurveRefType),
// `value` the percentage or the function number. ys[i] = the curve at xs[i],
// -1024..1024. Returns the number of values written. *added*
ETX_EXPORT(etx_apply_curve_ref)
int32_t etx_apply_curve_ref(int32_t type, int32_t value, const int16_t* xs, int16_t* ys,
                            int32_t n)
{
  if (!xs || !ys || n <= 0) return 0;
  if (type != CURVE_REF_DIFF && type != CURVE_REF_EXPO && type != CURVE_REF_FUNC) return 0;
  if (value < -100 || value > 100) return 0;
  CurveRef ref;
  ref.type = type;
  ref.value = makeSourceNumVal(value);
  for (int32_t i = 0; i < n; i++) ys[i] = (int16_t)applyCurve(xs[i], ref);
  return n;
}

ETX_EXPORT(etx_poll_events) int32_t etx_poll_events(char* out, int32_t max)
{
  if (s_events.empty() || !out || max <= 0) return 0;
  size_t n = s_events.size();
  if (n > (size_t)max) {
    // Whole lines only.
    size_t cut = s_events.rfind('\n', (size_t)max - 1);
    if (cut == std::string::npos) return 0;
    n = cut + 1;
  }
  memcpy(out, s_events.data(), n);
  s_events.erase(0, n);
  return (int32_t)n;
}
