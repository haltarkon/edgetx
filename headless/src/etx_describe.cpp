// SPDX-License-Identifier: GPL-2.0-or-later
//
// etx_describe_json() and etx_schema_json(): what this radio build is, read
// from EdgeTX itself.
//
// Every name here comes from EdgeTX's own tables and functions: YAML tokens
// from the model YAML writer's encoders (reached through the schema nodes that
// carry them), display labels from getSourceString()/getSwitchPositionName()
// and the English string tables, hardware names from the generated hardware
// tables, and the schema from the generated YamlNode tree
// (storage/yaml/yaml_datastructs_<radio>.cpp).

#include "etx_host_inputs.h"
#include "etx_json.h"
#include "etx_port_impl.h"

#include "edgetx.h"
#include "analogs.h"
#include "gvars.h"
#include "hal/adc_driver.h"
#include "hal/key_driver.h"
#include "hal/switch_driver.h"
#include "input_mapping.h"
#include "stamp.h"
#include "storage/yaml/yaml_datastructs.h"
#include "storage/yaml/yaml_node.h"
#include "switches.h"

#include <map>
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
#ifndef ETX_BASE_NAME
#define ETX_BASE_NAME FLAVOUR
#endif

#if defined(ETX_NO_EXPORT_ATTR)  // size analysis builds: exports chosen at link time
#define ETX_EXPORT(name) extern "C"
#else
#define ETX_EXPORT(name) extern "C" __attribute__((export_name(#name), used))
#endif

using etx::appendBool;
using etx::appendInt;
using etx::appendJsonString;
using etx::appendKey;

// ---- JSON strings -----------------------------------------------------------------

namespace etx {

static const struct {
  uint8_t code;  // second byte of "\302\2xx"
  const char* text;
  const char* icon;
} kGlyphs[] = {
    {0x80, "→", nullptr},  // CHAR_RIGHT
    {0x81, "←", nullptr},  // CHAR_LEFT
    {0x82, "↑", nullptr},  // CHAR_UP
    {0x83, "↓", nullptr},  // CHAR_DOWN
    {0x88, "Δ", nullptr},  // CHAR_DELTA
    {0x89, "", "stick"},       {0x8a, "", "pot"},      {0x8b, "", "slider"},
    {0x8c, "", "switch"},      {0x8d, "", "trim"},     {0x8e, "", "input"},
    {0x8f, "", "function"},    {0x90, "", "cyclic"},   {0x91, "", "trainer"},
    {0x92, "", "channel"},     {0x93, "", "telemetry"}, {0x94, "", "lua"},
    {0x95, "", "logical"},     {0x96, "", "curve"},
};

static const char* glyphText(uint8_t code, const char** icon)
{
  for (auto& g : kGlyphs)
    if (g.code == code) {
      if (icon) *icon = g.icon;
      return g.text;
    }
  return nullptr;
}

const char* glyphIcon(const char* s)
{
  if (!s || (uint8_t)s[0] != 0xc2) return nullptr;
  const char* icon = nullptr;
  glyphText((uint8_t)s[1], &icon);
  return icon;
}

void appendJsonString(std::string& out, const char* s, size_t n)
{
  out.push_back('"');
  for (size_t i = 0; s && i < n && s[i]; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == 0xc2 && i + 1 < n && ((unsigned char)s[i + 1] & 0xe0) == 0x80) {
      const char* text = glyphText((unsigned char)s[i + 1], nullptr);
      if (text) {
        out += text;
        i++;
        continue;
      }
    }
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back((char)c);
        }
    }
  }
  out.push_back('"');
}

}  // namespace etx

// ---- schema navigation ------------------------------------------------------------

static const YamlNode* childrenOf(const YamlNode* node)
{
  if (!node) return nullptr;
  if (node->type == YDT_ARRAY || node->type == YDT_UNION) return node->u._array.child;
  return nullptr;
}

static const YamlNode* findChild(const YamlNode* node, const char* tag)
{
  for (const YamlNode* c = childrenOf(node); c && c->type != YDT_NONE; c++)
    if (c->tag && !strcmp(c->tag, tag)) return c;
  return nullptr;
}

static const YamlNode* modelNode(const char* parent, const char* tag)
{
  return findChild(findChild(get_modeldata_nodes(), parent), tag);
}

// Run a custom scalar encoder (a YAML_*_CUST node's writer) into a string.
static bool collect(void* opaque, const char* str, size_t len)
{
  ((std::string*)opaque)->append(str, len);
  return true;
}

static std::string encodeWith(const YamlNode* node, uint32_t raw)
{
  std::string s;
  if (node && node->u._cust.uint_to_cust) node->u._cust.uint_to_cust(node, raw, collect, &s);
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

static uint32_t decodeWith(const YamlNode* node, const std::string& s)
{
  if (!node || !node->u._cust.cust_to_uint) return 0;
  return node->u._cust.cust_to_uint(node, s.data(), (uint8_t)s.size());
}

// A field's raw bits for a signed value of the node's width.
static uint32_t rawSigned(const YamlNode* node, int32_t v)
{
  uint32_t mask = node->size >= 32 ? 0xffffffffu : ((1u << node->size) - 1);
  return (uint32_t)v & mask;
}

static int32_t fromRawSigned(const YamlNode* node, uint32_t raw)
{
  if (node->size >= 32) return (int32_t)raw;
  uint32_t sign = 1u << (node->size - 1);
  raw &= (sign << 1) - 1;
  return (raw & sign) ? (int32_t)(raw | ~((sign << 1) - 1)) : (int32_t)raw;
}

static void appendEnumTable(std::string& out, const YamlIdStr* choices, const char* const* labels,
                            int labelCount)
{
  out.push_back('[');
  bool first = true;
  for (const YamlIdStr* e = choices; e && e->str; e++) {
    if (!first) out.push_back(',');
    first = false;
    out += "{\"value\":";
    appendInt(out, e->id);
    out += ",\"token\":";
    appendJsonString(out, e->str);
    if (labels && e->id >= 0 && e->id < labelCount) {
      out += ",\"label\":";
      appendJsonString(out, labels[e->id]);
    }
    out.push_back('}');
  }
  out.push_back(']');
}

static const YamlIdStr* enumOf(const YamlNode* node)
{
  return (node && node->type == YDT_ENUM) ? node->u._enum.choices : nullptr;
}

static void appendLabelList(std::string& out, const char* const* labels, int count)
{
  out.push_back('[');
  for (int i = 0; i < count; i++) {
    if (i) out.push_back(',');
    out += "{\"value\":";
    appendInt(out, i);
    out += ",\"label\":";
    appendJsonString(out, labels[i]);
    out.push_back('}');
  }
  out.push_back(']');
}

// ---- describe ---------------------------------------------------------------------

static const char* hwTypeName(SwitchHwType t)
{
  switch (t) {
    case SWITCH_HW_2POS: return "2POS";
    case SWITCH_HW_3POS: return "3POS";
    case SWITCH_HW_ADC: return "ADC";
  }
  return "?";
}

static const char* switchConfigName(int c)
{
  switch (c) {
    case SWITCH_NONE: return "NONE";
    case SWITCH_TOGGLE: return "TOGGLE";
    case SWITCH_2POS: return "2POS";
    case SWITCH_3POS: return "3POS";
#if defined(FUNCTION_SWITCHES)
    case SWITCH_GLOBAL: return "GLOBAL";
#endif
  }
  return "?";
}

static const char* flexConfigName(int c)
{
  switch (c) {
    case FLEX_NONE: return "none";
    case FLEX_POT: return "without_detent";
    case FLEX_POT_CENTER: return "with_detent";
    case FLEX_SLIDER: return "slider";
    case FLEX_MULTIPOS: return "multipos_switch";
    case FLEX_AXIS_X: return "axis_x";
    case FLEX_AXIS_Y: return "axis_y";
    case FLEX_SWITCH: return "switch";
  }
  return "?";
}

// Groups of the source and switch source enumerations (dataconstants.h).
static const char* sourceGroup(int i)
{
  if (i == MIXSRC_NONE) return "none";
  if (i <= MIXSRC_LAST_INPUT) return "input";
#if defined(LUA_INPUTS)
  if (i <= MIXSRC_LAST_LUA) return "lua";
#endif
  if (i <= MIXSRC_LAST_STICK) return "stick";
  if (i <= MIXSRC_LAST_POT) return "pot";
#if defined(IMU)
  if (i >= MIXSRC_TILT_X && i <= MIXSRC_TILT_Y) return "tilt";
#endif
#if defined(PCBHORUS)
  if (i >= MIXSRC_FIRST_SPACEMOUSE && i <= MIXSRC_LAST_SPACEMOUSE) return "spacemouse";
#endif
#if defined(HOST_INPUTS)
  // What the control is on the host: pad_axis, mouse_axis, pad_button.
  if (i >= MIXSRC_FIRST_HOST_AXIS && i <= MIXSRC_LAST_HOST_AXIS)
    return etxHostAxisKind(i - MIXSRC_FIRST_HOST_AXIS)[0] == 'p' ? "pad_axis" : "mouse_axis";
  if (i >= MIXSRC_FIRST_HOST_BUTTON && i <= MIXSRC_LAST_HOST_BUTTON) return "pad_button";
#endif
  if (i == MIXSRC_MIN || i == MIXSRC_MAX) return "minmax";
#if defined(LUMINOSITY_SENSOR)
  if (i == MIXSRC_LIGHT) return "light";
#endif
  if (i >= MIXSRC_FIRST_HELI && i <= MIXSRC_LAST_HELI) return "heli";
  if (i >= MIXSRC_FIRST_TRIM && i <= MIXSRC_LAST_TRIM) return "trim";
  if (i >= MIXSRC_FIRST_SWITCH && i <= MIXSRC_LAST_SWITCH) return "switch";
#if defined(FUNCTION_SWITCHES)
  if (i >= MIXSRC_FIRST_CUSTOMSWITCH_GROUP && i <= MIXSRC_LAST_CUSTOMSWITCH_GROUP)
    return "switch_group";
#endif
  if (i >= MIXSRC_FIRST_LOGICAL_SWITCH && i <= MIXSRC_LAST_LOGICAL_SWITCH) return "logical";
  if (i >= MIXSRC_FIRST_TRAINER && i <= MIXSRC_LAST_TRAINER) return "trainer";
  if (i >= MIXSRC_FIRST_CH && i <= MIXSRC_LAST_CH) return "channel";
  if (i >= MIXSRC_FIRST_GVAR && i <= MIXSRC_LAST_GVAR) return "gvar";
  if (i >= MIXSRC_TX_VOLTAGE && i <= MIXSRC_TX_GPS) return "radio";
  if (i >= MIXSRC_FIRST_TIMER && i <= MIXSRC_LAST_TIMER) return "timer";
  if (i >= MIXSRC_FIRST_TELEM && i <= MIXSRC_LAST_TELEM) return "telemetry";
  return "other";
}

static const char* switchGroupName(int i)
{
  if (i == SWSRC_NONE) return "none";
  if (i <= SWSRC_LAST_SWITCH) return "switch";
  if (i <= SWSRC_LAST_MULTIPOS_SWITCH) return "multipos";
  if (i <= SWSRC_LAST_TRIM) return "trim";
#if defined(HOST_INPUTS)
  if (i <= SWSRC_LAST_HOST_BUTTON) {
    // pad_button, mouse_button, key
    const char* kind = etxHostButtonKind(i - SWSRC_FIRST_HOST_BUTTON);
    return kind[0] == 'p' ? "pad_button" : kind[0] == 'm' ? "mouse_button" : "key";
  }
#endif
  if (i <= SWSRC_LAST_LOGICAL_SWITCH) return "logical";
  if (i <= SWSRC_ONE) return "on";
  if (i <= SWSRC_LAST_FLIGHT_MODE) return "flight_mode";
  if (i < SWSRC_FIRST_SENSOR) return "radio";
  return "telemetry";
}

static void appendSources(std::string& out)
{
  const YamlNode* node = modelNode("mixData", "srcRaw");
  out += "[";
  for (int i = MIXSRC_NONE; i <= MIXSRC_LAST_TELEM; i++) {
    if (i) out.push_back(',');
    std::string token = encodeWith(node, rawSigned(node, i));
    bool roundTrip = fromRawSigned(node, decodeWith(node, token)) == i;
    // The pre-instantiated fixed-size variant, into a cleared buffer: for a
    // source that does not exist (a Lua output with no script) EdgeTX writes
    // nothing, and the shared static buffer would still hold the last label.
    char label[16] = {};
    getSourceString(label, i, true);
    out += "{\"i\":";
    appendInt(out, i);
    out += ",\"token\":";
    if (roundTrip) appendJsonString(out, token.c_str());
    else out += "null";
    out += ",\"label\":";
    appendJsonString(out, label);
    const char* icon = etx::glyphIcon(label);
    if (icon) {
      out += ",\"icon\":";
      appendJsonString(out, icon);
    }
    out += ",\"group\":";
    appendJsonString(out, sourceGroup(i));
    bool avail = isSourceAvailable(i);
#if defined(HOST_INPUTS)
    // The radio's own source menus (gui_common.cpp) know nothing of host
    // inputs; here they are what the radio is made of.
#if defined(ETX_VIRTUAL_RADIO)
    // What the base radio has that this one does not: its tilt sensor.
    if (!strcmp(sourceGroup(i), "tilt") || !strcmp(sourceGroup(i), "spacemouse")) avail = false;
#endif
    if (i >= MIXSRC_FIRST_HOST_AXIS && i <= MIXSRC_LAST_HOST_BUTTON) avail = true;
#endif
    out += ",\"avail\":";
    appendBool(out, avail);
    out.push_back('}');
  }
  out += "]";
}

static void appendSwitchSources(std::string& out)
{
  const YamlNode* node = modelNode("mixData", "swtch");
  out += "[";
  for (int i = SWSRC_NONE; i < SWSRC_COUNT; i++) {
    if (i) out.push_back(',');
    std::string token = encodeWith(node, rawSigned(node, i));
    bool roundTrip = fromRawSigned(node, decodeWith(node, token)) == i;
    char* label = getSwitchPositionName(i, true);
    out += "{\"i\":";
    appendInt(out, i);
    out += ",\"token\":";
    if (roundTrip) appendJsonString(out, token.c_str());
    else out += "null";
    out += ",\"label\":";
    appendJsonString(out, label);
    out += ",\"group\":";
    appendJsonString(out, switchGroupName(i));
    // Where the radio's menus offer it (gui_common.cpp isSwitchAvailable()):
    // bit 0 mixes/inputs, bit 1 logical switches, bit 2 model special
    // functions, bit 3 radio (global) special functions.
    int ctx = (isSwitchAvailable(i, MixesContext) ? 1 : 0) |
              (isSwitchAvailable(i, LogicalSwitchesContext) ? 2 : 0) |
              (isSwitchAvailable(i, ModelCustomFunctionsContext) ? 4 : 0) |
              (isSwitchAvailable(i, GeneralCustomFunctionsContext) ? 8 : 0);
    out += ",\"avail\":";
    appendInt(out, ctx);
    out.push_back('}');
  }
  out += "]";
}

static void appendAnalogs(std::string& out)
{
  const YamlNode* src = modelNode("mixData", "srcRaw");
  int sticks = adcGetMaxInputs(ADC_INPUT_MAIN);
  int flex = adcGetMaxInputs(ADC_INPUT_FLEX);
  out += "[";
  for (int i = 0; i < sticks + flex; i++) {
    if (i) out.push_back(',');
    bool isStick = i < sticks;
    int type = isStick ? ADC_INPUT_MAIN : ADC_INPUT_FLEX;
    int idx = isStick ? i : i - sticks;
    int source = isStick ? MIXSRC_FIRST_STICK + inputMappingConvertMode(i) : MIXSRC_FIRST_POT + idx;
    out += "{\"index\":";
    appendInt(out, i);
    out += ",\"name\":";
    appendJsonString(out, adcGetInputName(type, idx));
    // A stick is labelled by the role the stick mode gives it (Mode 2: LV is Thr).
    out += ",\"label\":";
    appendJsonString(out, isStick ? getMainControlLabel(inputMappingConvertMode(i), true)
                                  : getPotLabel(idx, true));
    out += ",\"type\":";
    if (isStick) {
      appendJsonString(out, "stick");
    } else {
      int cfg = getPotType(idx);
      appendJsonString(out, cfg == FLEX_MULTIPOS                          ? "multipos"
                            : cfg == FLEX_SLIDER                          ? "slider"
                            : cfg == FLEX_SWITCH                          ? "switch"
                            : cfg == FLEX_NONE                            ? "none"
                            : (cfg == FLEX_AXIS_X || cfg == FLEX_AXIS_Y) ? "axis"
                                                                          : "pot");
      out += ",\"config\":";
      appendJsonString(out, flexConfigName(cfg));
      out += ",\"defaultConfig\":";
      appendJsonString(out,
                       flexConfigName((adcGetDefaultPotsConfig() >> (POT_CFG_BITS * idx)) &
                                      POT_CFG_MASK));
    }
    out += ",\"inverted\":";
    appendBool(out, isStick ? getStickInversion(inputMappingConvertMode(i)) : getPotInversion(idx));
    out += ",\"source\":";
    appendInt(out, source);
    out += ",\"token\":";
    appendJsonString(out, encodeWith(src, rawSigned(src, source)).c_str());
    out.push_back('}');
  }
  out += "]";
}

static void appendSwitches(std::string& out)
{
  int physical = switchGetMaxSwitches();
  int all = switchGetMaxAllSwitches();
#if defined(ETX_VIRTUAL_RADIO)
  // The base radio's flex switches are pots set up as switches: no pots here.
  all = physical;
#endif
  out += "[";
  for (int i = 0; i < all; i++) {
    if (i) out.push_back(',');
    out += "{\"index\":";
    appendInt(out, i);
    out += ",\"name\":";
    appendJsonString(out, switchGetDefaultName(i));
    out += ",\"hw\":";
    appendJsonString(out, hwTypeName(switchGetHwType(i)));
    out += ",\"config\":";
    appendJsonString(out, switchConfigName(g_eeGeneral.switchType(i)));
    out += ",\"modelConfig\":";
    appendJsonString(out, switchConfigName(g_model.getSwitchType(i)));
    if (i < physical) {
      out += ",\"default\":";
      appendJsonString(out, switchConfigName(switchGetDefaultConfig(i)));
    }
    out += ",\"settable\":";
    appendBool(out, i < physical);
    out += ",\"flex\":";
    appendBool(out, switchIsFlex(i));
    bool cfs = switchIsCustomSwitch(i);
    out += ",\"cfs\":";
    appendBool(out, cfs);
#if defined(FUNCTION_SWITCHES)
    if (cfs) {
      out += ",\"cfsIndex\":";
      appendInt(out, switchGetCustomSwitchIdx(i));
    }
#endif
    out.push_back('}');
  }
  out += "]";
}

static void appendTrims(std::string& out)
{
  const YamlNode* src = modelNode("mixData", "srcRaw");
  int n = keysGetMaxTrims();
  int sticks = adcGetMaxInputs(ADC_INPUT_MAIN);
  out += "[";
  for (int i = 0; i < n; i++) {
    if (i) out.push_back(',');
    // The buttons are wired to the physical sticks; the value they move is
    // the model's trim for the stick's role under the current stick mode.
    int modelTrim = i < sticks ? inputMappingConvertMode(i) : i;
    char name[8];
    snprintf(name, sizeof(name), "T%d", i + 1);
    out += "{\"index\":";
    appendInt(out, i);
    out += ",\"name\":";
    appendJsonString(out, name);
    out += ",\"modelTrim\":";
    appendInt(out, modelTrim);
    out += ",\"label\":";
    appendJsonString(out, getTrimLabel(modelTrim, true));
    out += ",\"token\":";
    appendJsonString(out,
                     encodeWith(src, rawSigned(src, MIXSRC_FIRST_TRIM + modelTrim)).c_str());
    out.push_back('}');
  }
  out += "]";
}

// The host inputs (hal/host_inputs.h): `index` for etx_set_host_axis /
// etx_set_host_button, `name` the YAML token (a key's is its
// KeyboardEvent.code), `kind` what it is on the host and `n` its number there
// (a controller's axis or button number from 0, MouseEvent.button, the
// position among the keys), and the mixer `source` / `switchSource` it is.
static void appendHostAxes(std::string& out)
{
  out += "[";
#if defined(HOST_INPUTS)
  for (int i = 0; i < hostInputsGetMaxAxes(); i++) {
    if (i) out.push_back(',');
    out += "{\"index\":";
    appendInt(out, i);
    out += ",\"name\":";
    appendJsonString(out, hostAxisGetName(i));
    out += ",\"label\":";
    appendJsonString(out, hostAxisGetLabel(i));
    out += ",\"kind\":";
    appendJsonString(out, etxHostAxisKind(i));
    out += ",\"n\":";
    appendInt(out, etxHostAxisOrdinal(i));
    out += ",\"source\":";
    appendInt(out, MIXSRC_FIRST_HOST_AXIS + i);
    out.push_back('}');
  }
#endif
  out += "]";
}

static void appendHostButtons(std::string& out)
{
  out += "[";
#if defined(HOST_INPUTS)
  for (int i = 0; i < hostInputsGetMaxButtons(); i++) {
    if (i) out.push_back(',');
    out += "{\"index\":";
    appendInt(out, i);
    out += ",\"name\":";
    appendJsonString(out, hostButtonGetName(i));
    out += ",\"label\":";
    appendJsonString(out, hostButtonGetLabel(i));
    out += ",\"kind\":";
    appendJsonString(out, etxHostButtonKind(i));
    out += ",\"n\":";
    appendInt(out, etxHostButtonOrdinal(i));
    out += ",\"switchSource\":";
    appendInt(out, SWSRC_FIRST_HOST_BUTTON + i);
    if (i < MAX_HOST_ANALOG_BUTTONS) {
      out += ",\"source\":";
      appendInt(out, MIXSRC_FIRST_HOST_BUTTON + i);
    }
    out.push_back('}');
  }
#endif
  out += "]";
}

static void appendKeys(std::string& out)
{
  out += "[";
  bool first = true;
  for (int k = 0; k < MAX_KEYS; k++) {
    if (!keyIsSupported((EnumKeys)k)) continue;
    if (!first) out.push_back(',');
    first = false;
    out += "{\"index\":";
    appendInt(out, k);
    out += ",\"label\":";
    appendJsonString(out, keysGetLabel((EnumKeys)k));
    out.push_back('}');
  }
  out += "]";
}

static std::string describe()
{
  std::string out;
  out.reserve(96 * 1024);
  out += "{\"api\":";
  appendInt(out, ETX_API_VERSION);
  out += ",\"edgetx\":{\"commit\":";
  appendJsonString(out, ETX_EDGETX_COMMIT);
  out += ",\"version\":";
  appendJsonString(out, VERSION);
  out += "},\"radio\":{\"target\":";
  appendJsonString(out, ETX_TARGET_NAME);
  // The radio EdgeTX is configured as, and what it writes as a radio.yml's
  // `board:` -- the same on a real radio; the virtual radio is configured as
  // one and writes its own name.
  out += ",\"flavour\":";
  appendJsonString(out, ETX_BASE_NAME);
  out += ",\"board\":";
  appendJsonString(out, FLAVOUR);
  out += ",\"lcd\":{\"w\":";
  appendInt(out, LCD_W);
  out += ",\"h\":";
  appendInt(out, LCD_H);
  out += "},\"colour\":";
#if defined(COLORLCD)
  appendBool(out, true);
#else
  appendBool(out, false);
#endif
  out += ",\"surface\":";
#if defined(SURFACE_RADIO)
  appendBool(out, true);
#else
  appendBool(out, false);
#endif
  out += ",\"stickMode\":";
  appendInt(out, g_eeGeneral.stickMode);
  out += ",\"channelOrder\":";
  appendInt(out, g_eeGeneral.templateSetup);
  out += ",\"channelOrders\":";
  appendInt(out, inputMappingGetMaxChannelOrder());
  out += ",\"throttleStick\":";
  appendInt(out, inputMappingGetThrottle());
  // A radio that exists only here: its pots and switches are host inputs.
  out += ",\"virtual\":";
#if defined(ETX_VIRTUAL_RADIO)
  appendBool(out, true);
#else
  appendBool(out, false);
#endif
  out += "}";

  out += ",\"capacities\":{";
  struct { const char* k; int v; } caps[] = {
      {"outputs", MAX_OUTPUT_CHANNELS},
      {"mixes", MAX_MIXERS},
      {"expos", MAX_EXPOS},
      {"inputs", MAX_INPUTS},
      {"logicalSwitches", MAX_LOGICAL_SWITCHES},
      {"specialFunctions", MAX_SPECIAL_FUNCTIONS},
#if defined(GVARS)
      {"gvars", MAX_GVARS},
#else
      {"gvars", 0},
#endif
      {"flightModes", MAX_FLIGHT_MODES},
      {"curves", MAX_CURVES},
      {"curvePoints", MAX_CURVE_POINTS},
      {"pointsPerCurve", MAX_POINTS_PER_CURVE},
      {"timers", MAX_TIMERS},
      {"trims", keysGetMaxTrims()},
      {"sticks", adcGetMaxInputs(ADC_INPUT_MAIN)},
      {"flexInputs", adcGetMaxInputs(ADC_INPUT_FLEX)},
      {"switches", switchGetMaxSwitches()},
#if defined(ETX_VIRTUAL_RADIO)
      // The base radio's flex switches are pots set up as switches: no pots here.
      {"allSwitches", switchGetMaxSwitches()},
      {"flexSwitches", 0},
#else
      {"allSwitches", switchGetMaxAllSwitches()},
      {"flexSwitches", MAX_FLEX_SWITCHES},
#endif
      {"functionSwitches", NUM_FUNCTIONS_SWITCHES},
#if defined(FUNCTION_SWITCHES)
      {"functionSwitchGroups", NUM_FUNCTIONS_GROUPS},
#else
      {"functionSwitchGroups", 0},
#endif
      {"telemetrySensors", MAX_TELEMETRY_SENSORS},
      {"scripts", MAX_SCRIPTS},
      {"trainerChannels", MAX_TRAINER_CHANNELS},
      {"multiposPositions", XPOTS_MULTIPOS_COUNT},
  };
  for (size_t i = 0; i < DIM(caps); i++) {
    if (i) out.push_back(',');
    appendKey(out, caps[i].k);
    appendInt(out, caps[i].v);
  }
  out += "}";

  out += ",\"limits\":{\"trim\":";
  appendInt(out, TRIM_MAX);
  out += ",\"trimExtended\":";
  appendInt(out, TRIM_EXTENDED_MAX);
  out += ",\"channelStd\":";
  appendInt(out, LIMIT_STD_MAX);
  out += ",\"channelExt\":";
  appendInt(out, LIMIT_EXT_MAX);
  out += ",\"ppmCenter\":";
  appendInt(out, PPM_CENTER);
  out += ",\"resx\":";
  appendInt(out, RESX);
  out += "}";

  out += ",\"analogs\":";
  appendAnalogs(out);
  out += ",\"switches\":";
  appendSwitches(out);
  out += ",\"trims\":";
  appendTrims(out);
  out += ",\"keys\":";
  appendKeys(out);
  out += ",\"hostAxes\":";
  appendHostAxes(out);
  out += ",\"hostButtons\":";
  appendHostButtons(out);
  out += ",\"sources\":";
  appendSources(out);
  out += ",\"switchSources\":";
  appendSwitchSources(out);

  out += ",\"enums\":{\"logicalSwitchFunctions\":";
  appendEnumTable(out, enumOf(modelNode("logicalSw", "func")), STR_VCSWFUNC, LS_FUNC_COUNT);
  out += ",\"specialFunctions\":";
  {
    const YamlIdStr* e = enumOf(modelNode("customFn", "func"));
    out.push_back('[');
    bool first = true;
    for (; e && e->str; e++) {
      if (!first) out.push_back(',');
      first = false;
      out += "{\"value\":";
      appendInt(out, e->id);
      out += ",\"token\":";
      appendJsonString(out, e->str);
      out += ",\"label\":";
      appendJsonString(out, funcGetLabel(e->id));
      out.push_back('}');
    }
    out.push_back(']');
  }
  out += ",\"mixMultiplex\":";
  appendEnumTable(out, enumOf(modelNode("mixData", "mltpx")), STR_VMLTPX, 3);
  out += ",\"timerModes\":";
  appendEnumTable(out, enumOf(modelNode("timers", "mode")), STR_VTMRMODES, TMRMODE_COUNT);
  out += ",\"swashTypes\":";
  appendEnumTable(out, enumOf(findChild(get_modeldata_nodes(), "swashR") ?
                                  findChild(findChild(get_modeldata_nodes(), "swashR"), "type")
                                  : nullptr),
                  nullptr, 0);
  out += ",\"curveRefTypes\":";
  appendLabelList(out, STR_VCURVETYPE, CURVE_REF_CUSTOM + 1);
  out += ",\"curveFunctions\":";
  appendLabelList(out, STR_VCURVEFUNC, 7);
  out += ",\"curveTypes\":";
  appendLabelList(out, STR_CURVE_TYPES, CURVE_TYPE_LAST + 1);
  // The unit a telemetry sensor's value is shown with (STR_VTELEMUNIT, by the
  // sensor's `unit`; getValueWithUnit() shows UNIT_RAW, value 0, without one).
  out += ",\"telemetryUnits\":";
  appendLabelList(out, STR_VTELEMUNIT, UNIT_MAX + 1);
  out += ",\"sounds\":";
  {
    // PLAY_SOUND parameter i plays system sound AU_SPECIAL_SOUND_FIRST + i
    // (the "sound" event id).
    out.push_back('[');
    for (int i = 0; i < AU_SPECIAL_SOUND_LAST - AU_SPECIAL_SOUND_FIRST; i++) {
      if (i) out.push_back(',');
      out += "{\"value\":";
      appendInt(out, i);
      out += ",\"id\":";
      appendInt(out, AU_SPECIAL_SOUND_FIRST + i);
      out += ",\"label\":";
      appendJsonString(out, STR_FUNCSOUNDS[i]);
      out.push_back('}');
    }
    out.push_back(']');
  }
  out += "}}";
  return out;
}

ETX_EXPORT(etx_describe_json) int32_t etx_describe_json(char* out, int32_t max)
{
  return etx::copyOut(describe(), out, max);
}

// ---- schema ---------------------------------------------------------------------

static const char* nodeTypeName(uint8_t t)
{
  switch (t) {
    case YDT_IDX: return "idx";
    case YDT_SIGNED: return "signed";
    case YDT_UNSIGNED: return "unsigned";
    case YDT_STRING: return "string";
    case YDT_ARRAY: return "array";
    case YDT_ENUM: return "enum";
    case YDT_UNION: return "union";
    case YDT_PADDING: return "padding";
    case YDT_CUSTOM: return "custom";
  }
  return "none";
}

struct SchemaWriter {
  std::string out;
  std::map<const YamlIdStr*, int> enums;
  std::map<const YamlNode*, int> structs;
  std::string structsJson;

  int enumId(const YamlIdStr* e)
  {
    auto it = enums.find(e);
    if (it != enums.end()) return it->second;
    int id = (int)enums.size();
    enums[e] = id;
    return id;
  }

  void node(std::string& o, const YamlNode* n)
  {
    o += "{\"tag\":";
    if (n->tag) appendJsonString(o, n->tag);
    else o += "null";
    o += ",\"type\":";
    appendJsonString(o, nodeTypeName(n->type));
    o += ",\"bits\":";
    appendInt(o, n->size);
    switch (n->type) {
      case YDT_ARRAY:
      case YDT_UNION: {
        if (n->type == YDT_ARRAY) {
          o += ",\"elmts\":";
          appendInt(o, n->elmts);
          const YamlNode* first = n->u._array.child;
          const char* kind = n->elmts == 1 ? "struct"
                             : (first && first->type == YDT_IDX) ? "keyed"
                                                                  : "list";
          o += ",\"kind\":";
          appendJsonString(o, kind);
          if (n->u._array.u.is_active) o += ",\"conditional\":true";
        } else if (n->u._array.u.select_member) {
          o += ",\"selected\":true";
        }
        o += ",\"struct\":";
        appendInt(o, structId(n->u._array.child));
        break;
      }
      case YDT_ENUM:
        o += ",\"enum\":";
        appendInt(o, enumId(n->u._enum.choices));
        if (n->u._enum.is_active) o += ",\"conditional\":true";
        break;
      case YDT_SIGNED:
      case YDT_UNSIGNED:
        if (n->u._cust.cust_to_uint || n->u._cust.uint_to_cust) o += ",\"custom\":true";
        break;
      case YDT_IDX:
        if (n->u._cust_idx.read || n->u._cust_idx.write) o += ",\"custom\":true";
        break;
      case YDT_STRING:
        o += ",\"length\":";
        appendInt(o, n->size / 8);
        break;
      default:
        break;
    }
    o += "}";
  }

  // Structs (the child lists of arrays and unions) are emitted once each and
  // referenced by number, as the generated tables share them.
  int structId(const YamlNode* children)
  {
    auto it = structs.find(children);
    if (it != structs.end()) return it->second;
    int id = (int)structs.size();
    structs[children] = id;
    std::string s = "[";
    bool first = true;
    for (const YamlNode* c = children; c && c->type != YDT_NONE; c++) {
      if (!first) s.push_back(',');
      first = false;
      node(s, c);
    }
    s += "]";
    if (!structsJson.empty()) structsJson.push_back(',');
    structsJson += "\"";
    structsJson += std::to_string(id);
    structsJson += "\":";
    structsJson += s;
    return id;
  }

  std::string run()
  {
    std::string roots = "{\"model\":";
    node(roots, get_modeldata_nodes());
    roots += ",\"radio\":";
    node(roots, get_radiodata_nodes());
    roots += "}";

    std::string o = "{\"api\":";
    appendInt(o, ETX_API_VERSION);
    o += ",\"target\":";
    appendJsonString(o, ETX_TARGET_NAME);
    o += ",\"roots\":";
    o += roots;
    o += ",\"structs\":{";
    o += structsJson;
    o += "},\"enums\":{";
    bool first = true;
    for (auto& kv : enums) {
      if (!first) o.push_back(',');
      first = false;
      o += "\"";
      o += std::to_string(kv.second);
      o += "\":";
      appendEnumTable(o, kv.first, nullptr, 0);
    }
    o += "}}";
    return o;
  }
};

ETX_EXPORT(etx_schema_json) int32_t etx_schema_json(char* out, int32_t max)
{
  SchemaWriter w;
  return etx::copyOut(w.run(), out, max);
}
