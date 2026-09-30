# edgetx-headless — EdgeTX's mixer as a WebAssembly module

This directory builds **EdgeTX itself** — the radio firmware in this repository — into a small
WebAssembly module per radio, without its screen, its tasks or its SD card: the model YAML reader
and writer, the analog/switch/trim input pipeline, inputs, mixes, curves, outputs, logical
switches, special functions, global variables, flight modes, trims and timers, exactly as the
radio runs them. ArduConfigurator's Joystick tab drives it as a virtual EdgeTX radio (a gamepad
moves its sticks and switches) and sends the channels it computes to an ArduPilot vehicle.

Nothing in the mixer is reimplemented. The files here only decide *when* EdgeTX runs what (in
the order the radio's 10 ms interrupt, mixer task and UI task run it), stand in for the hardware
(a virtual board), and expose a flat C API.

| file | what it is |
|---|---|
| `cmake/inject.cmake` | the CMake glue injected into EdgeTX's own build (see *How it is built*) |
| `src/etx_api.cpp` | the C API, the tick, the event log, link-time wrappers |
| `src/etx_describe.cpp` | `etx_describe_json` / `etx_schema_json` |
| `src/etx_board.cpp` | the virtual board: ADC, switches, keys, trims, the rest of the simulator's board stubs |
| `src/etx_fs.cpp` | memory files behind EdgeTX's FatFs calls |
| `src/etx_port.h`, `src/etx_port.cpp` | a single-threaded port of EdgeTX's `os/` layer |
| `src/etx_json.h`, `src/etx_port_impl.h`, `src/etx_link.cpp` | internal helpers |

The JavaScript side lives in ArduConfigurator: `src/joystick/edgetx/engine.js` (loader, WASI shim,
typed API), with `scripts/edgetx-wasm.mjs` / `scripts/edgetx-wasm-smoke.mjs` to build and
exercise it.

## This branch

This is the `headless` branch of <https://github.com/haltarkon/edgetx-wasm>, a fork of
[EdgeTX](https://github.com/EdgeTX/edgetx). The branch is upstream EdgeTX plus this directory and
nothing else: **no upstream file is modified** — the build is hooked into EdgeTX's own CMake
project from outside (*How it is built*) — so the branch rebases onto any newer upstream EdgeTX
without conflicts. Whether the glue still builds against that EdgeTX is for the build to say (a
newer EdgeTX that makes code reachable which the headless link leaves out shows up as a non-WASI
import, step 3 of *How it is built*).

## Building

### In ArduConfigurator

ArduConfigurator pins this branch as its `vendor/edgetx` submodule and builds the modules with its
own `scripts/edgetx-wasm.mjs`, from the app's checkout:

```bash
npm run edgetx:wasm -- --setup     # once: WASI SDK 25.0 (SHA-256 pinned) + a Python venv
npm run edgetx:wasm                # tx16s and gx12 -> public/edgetx/edgetx-<radio>.wasm
npm run edgetx:wasm -- gx12        # one radio (any name tools/build-common.sh knows)
npm run edgetx:wasm -- --package   # + public/edgetx/manifest.json (size, SHA-256, commit, API)
npm run edgetx:wasm -- --out dir   # somewhere else;  --jobs n  compile parallelism (default 3)
npm run edgetx:wasm:smoke          # drive every built module end to end under Node
npx vitest run test/edgetxEngine.test.js   # the same checks as unit tests (skipped when not built)
```

Requirements: CMake ≥ 3.21, `make`, `tar`, Python 3 (for the venv; `ARDU_PYTHON` picks the
interpreter), network access for `--setup`. `vendor/edgetx` must be checked out with its nested
submodules (`git -C vendor/edgetx submodule update --init --recursive`).

Everything the build writes lives in the app's `node_modules/.cache/edgetx-wasm/` (override with
`ARDU_EDGETX_WASM_CACHE`): `wasi-sdk-25.0-<host>/`, `python/`, `build-<radio>/`,
`fetchcontent/`. Nothing is written inside the EdgeTX checkout.

The modules and their manifest **are committed** to ArduConfigurator (`public/edgetx/`, mode
0644 — the build script clears the executable bit the linker sets), unlike its SITL builds.
Nothing downstream builds them: `vendor/edgetx` is an `update = none` submodule, so neither
Cloudflare's build clone nor CI fetches this repository, and the EdgeTX mode of the Joystick tab
needs them in every build — the hosted one, the desktop installers and a plain checkout's dev
server. They are small (about 200 KB per radio, 80 KB gzipped), reproducible — builds contain no
dates or local paths, and the manifest names the commit of this branch each one was built from —
and change only when the pinned commit does, so the binary history they add is a few hundred
kilobytes per EdgeTX update. ArduConfigurator's `test/edgetxModules.test.js` keeps them honest:
every module is listed in the manifest with its size and SHA-256, the API matches `engine.js`,
the manifest's commit is the one `vendor/edgetx` is pinned at and, where the submodule is checked
out, its glue digest is this directory's — so moving the pin or editing the glue without
rebuilding (`npm run edgetx:wasm -- --package`) fails `npm test`.

A change to the glue is therefore made here: commit it on `headless`, push the branch to
<https://github.com/haltarkon/edgetx-wasm>, then bump the `vendor/edgetx` gitlink in
ArduConfigurator and rebuild with `npm run edgetx:wasm -- --package`, which refuses a checkout
with local changes (the manifest could otherwise only name a commit the modules were not built
from).

### Without ArduConfigurator

The build script only drives EdgeTX's CMake; in a checkout of this branch the same modules come
out of plain commands (`x86_64-linux` stands for the host's WASI SDK archive: `arm64-linux`,
`x86_64-macos`, `arm64-macos`):

```bash
git submodule update --init --recursive     # EdgeTX's own third-party submodules

TOOLS=$HOME/.cache/edgetx-headless          # tools and build trees, outside the checkout
mkdir -p "$TOOLS"
# WASI SDK 25.0, the release cmake/FetchWasiSDK.cmake names
curl -L https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-25/wasi-sdk-25.0-x86_64-linux.tar.gz \
  | tar -xz -C "$TOOLS"
SDK=$TOOLS/wasi-sdk-25.0-x86_64-linux
# the Python packages EdgeTX's generators run in a radio build (hardware tables, fonts, bitmaps)
python3 -m venv "$TOOLS/python"
"$TOOLS/python/bin/pip" install jinja2==3.1.6 MarkupSafe==3.0.3 pydantic==2.13.5 \
  pydantic_core==2.46.5 annotated-types==0.8.0 typing-inspection==0.4.4 \
  typing_extensions==4.16.0 pillow==12.3.0 lz4==4.4.5

# one radio, with the options tools/build-common.sh gives it (gx12: -DPCB=X7 -DPCBREV=GX12,
# tx16s: -DPCB=X10 -DPCBREV=TX16S)
cmake -S . -B "$TOOLS/build-gx12" \
  -DCMAKE_TOOLCHAIN_FILE="$SDK/share/cmake/wasi-sdk-p1.cmake" -DWASI_SDK_PREFIX="$SDK" \
  -DCMAKE_BUILD_TYPE=Release -DEdgeTX_SUPERBUILD=OFF -DNATIVE_BUILD=ON -DETX_API_VERSION=1 \
  -DPCB=X7 -DPCBREV=GX12 \
  -DCMAKE_PROJECT_EdgeTX_INCLUDE="$PWD/headless/cmake/inject.cmake" \
  -DPython3_EXECUTABLE="$TOOLS/python/bin/python3" \
  -DETX_EDGETX_COMMIT="$(git rev-parse HEAD | cut -c1-10)"
cmake --build "$TOOLS/build-gx12" --target edgetx-headless --parallel 3
# -> $TOOLS/build-gx12/edgetx-gx12.wasm
```

From the commit a manifest names, with these options, the result is byte for byte the module
whose SHA-256 that manifest records.

## How it is built

The build (ArduConfigurator's `scripts/edgetx-wasm.mjs`, or the commands above) configures
**EdgeTX's own CMake project** for one radio with:

- the options `tools/build-common.sh` gives that radio (`tx16s` → `-DPCB=X10 -DPCBREV=TX16S`,
  `gx12` → `-DPCB=X7 -DPCBREV=GX12`), read from the script itself;
- the native (simulator) build EdgeTX's own WebAssembly simulator uses: `-DNATIVE_BUILD=ON
  -DEdgeTX_SUPERBUILD=OFF`, Release, with the WASI SDK's **`wasm32-wasip1`** toolchain (no
  threads);
- `-DCMAKE_PROJECT_EdgeTX_INCLUDE=headless/cmake/inject.cmake`.

`project(EdgeTX)` includes `inject.cmake`, which schedules a call (`cmake_language(DEFER)`) for the
end of the top-level directory. By then `radio/src` has declared `radiolib_native` (every radio
source as an object library, with that radio's definitions) and `simu_drivers`, and the call:

1. **retargets them to a single-threaded port**: removes `-DNATIVE_THREADS` and the
   `os/*_native.cpp` implementation (which is `std::thread`), and force-includes
   `src/etx_port.h`, which supplies the task/mutex/timer handle types EdgeTX's `os/*.h` expect
   from a port. `etx_port.cpp` implements them: no task ever starts, mutexes are uncontended,
   software timers never fire, `time_get_ms()` is the virtual clock;
2. keeps only the plain stub files of `simu_drivers` — `simulib`, `audio_driver`,
   `adc_driver`, `switch_driver` carry the simulator's `export_name`/`import_name` attributes
   (which would export the simulator's API and keep its whole firmware alive) and `simufatfs`,
   `simudisk` use the host filesystem; `etx_board.cpp` and `etx_fs.cpp` replace them;
3. on colour radios leaves the **colour GUI out of the link** (`gui/colorlcd`, LVGL and its fonts,
   Lua's LVGL bindings), except the three files that implement the model's screen/widget data
   classes the YAML code uses (`mainview/layout.cpp`, `topbar.cpp`, `widget.cpp`). Colour pages
   register themselves from static constructors, and wasm-ld keeps the constructors of every
   object that supplies one live symbol — even an inline `std::string` member it happened to take
   from a GUI object — so linking the GUI at all pulls most of it in (3.8 MB instead of 0.2 MB).
   The link uses `--import-undefined`, and ArduConfigurator's build script fails if the module
   imports anything but WASI: anything left out that is reachable after all shows up there;
4. adds the glue sources to `radiolib_native` itself, so they compile with exactly the radio's
   definitions, include paths and flags (the structures are shared with EdgeTX code);
5. archives everything and links `edgetx-<radio>.wasm`, a WASI **reactor**
   (`-mexec-model=reactor`, `--gc-sections`, `--strip-all`): only what the API reaches is
   linked. The exports are the `etx_*` API, `memory`, `_initialize`, `malloc` and `free`;
6. applies **link-time wrappers** (`--wrap`) to a few EdgeTX functions:

| wrapped | why |
|---|---|
| `audioEvent`, `audioPlay`, `audioTrimPress`, `audioTimerCountdown`, `playModelEvent` | reported as events, then EdgeTX continues (and maps them to tones/files) |
| `AudioQueue::playFile`, `AudioQueue::playTone` | reported, not queued — nothing plays, so EdgeTX sees every sound finished at once |
| `hapticQueue::event`, `hapticQueue::play` | reported (a simulator build only counts haptic events) |
| `checkAll` | the pre-flight check screens (throttle, switches) would block; reported instead |
| `POPUP_WARNING`, `POPUP_WARNING_ON_UI_TASK`, `POPUP_BUBBLE` | pop-ups become events |
| `writeScreenshot`, `setRequestedMainView`, `showTelemScreen` | SCREENSHOT / SET_SCREEN become events |
| `luaExec` | the B&W model wizard (`setModelDefaults`) — no SD card, no Lua scripts here |
| `menuMainView`, `menuViewTelemetry`, `menuChannelsView` (B&W) | INSTANT_TRIM tests which menu is shown; the stand-ins keep the menu system out and `etx_init` puts the main view "on screen" |
| `LayoutFactory::deleteCustomScreens/TopBarWidgets`, `ViewMain::instance/getCurrentMainView` (colour) | a new model tears down screens; the radio-settings writer asks for the current screen: there are none |

No EdgeTX source file is modified. The one piece of EdgeTX logic restated here is
`sortMixerLines()` (file-static in `storage/storage_common.cpp`): the post-load sort of mix lines
by channel, a dozen lines.

## The tick

`etx_step(ms)` advances in 10 ms ticks (a remainder is carried). Each tick runs, in order:

1. `per10ms()` — the radio's 10 ms interrupt: `g_tmr10ms`, key and trim-button polling with
   EdgeTX's debounce and auto-repeat, function switches, telemetry timers;
2. the mixer task's body: `doMixerCalculations()` (ADC through EdgeTX's pipeline, switch
   positions, `evalMixes`: inputs, flight mode and fades, logical switches, mixes, special
   functions, limits) and `doMixerPeriodicUpdates()` (timers, logical-switch delays/durations,
   trim buttons → `checkTrims`);
3. every fifth tick, the part of the UI task's `perMain()` that affects outputs and effects:
   `evalUIFunctions()` for radio and model special functions (volume, backlight, logging, flight
   reset, screenshot, set screen).

Pulses are not generated; `channelOutputs` (what they are made from) and the PPM widths are
exposed instead.

### Differences from a real radio (by construction)

- **Inputs are exact.** `etx_set_analog` takes the *calibrated* position; a simulator build does
  not apply calibration, and the ADC jitter filter is bypassed (its state is primed each tick so
  every value passes unfiltered). Radio inversion settings, multipos quantisation, dead zone,
  throttle reversal and the 6POS debounce still apply, as EdgeTX code.
- The mixer runs once per 10 ms tick; the radio's mixer task runs more often, but everything
  time-based in the mixer (delays, slow, fades, timers, repeats) counts 10 ms ticks, so the
  results are the same.
- Key and trim-button debounce is one tick (`FILTERBITS 1` in simulator builds) instead of four:
  a trim press registers after 20 ms instead of 50 ms. A release, as on the radio, takes eight
  polls to register (EdgeTX keeps eight samples of each key): until then the key still counts as
  held, so repeats in progress continue for up to 70 ms after `etx_set_trim_key(t, 0)` and a
  second press of the same button within 80 ms of the release is not a new press.
- Nothing is played or shown: sounds, haptics, pop-ups and pre-flight checks are reported as
  events. A repeating PLAY_* function therefore repeats on its own schedule regardless of how long
  the sound would have played.
- Lua model scripts and telemetry are not run; trainer input is never valid; RF modules are not
  driven.
- On colour radios the post-load layout defaults (default screen, top-bar widgets) are not
  applied — they need the GUI; screen data in a file is read and written unchanged.

## C API

`etx_api_version()` changes only when an existing function changes meaning. Functions added
since (marked *added* below) keep the version: `engine.js` feature-detects them
(`engine.hasExport(name)`), so an older module still loads and simply lacks the feature.

All functions are `extern "C"`, called from one thread. Strings are UTF-8 in module memory
(allocate with `malloc`). Functions that produce text take `(char* out, int32 max)`, write at most
`max` bytes (plus a NUL when it fits) and **return the full length** — call again with a larger
buffer when the result exceeds `max`. Array getters take `(T* out, int32 max)` and return the
number of elements written (none for `max` ≤ 0).

| function | |
|---|---|
| `int32 etx_api_version()` | 1 |
| `int32 etx_init()` | radio defaults for this build (as with no `radio.yml`), stick mode 2, sticks centred, a new default model, runtime reset |
| `int32 etx_last_error(char*, int32)` | text of the last load error |
| `int32 etx_set_stick_mode(int32 mode)` | 0..3 (Mode 1..4); returns the previous |
| `int32 etx_load_radio_yaml(const char*, int32 len)` | EdgeTX's radio settings reader (`radio.yml`: stick mode, switch and pot types, names, inversion...), with the defaults `loadRadioSettings()` applies first and `postRadioSettingsLoad()`; the checksum is not enforced. Every read starts from a boot's state, the flex switches' pots included (kept outside `g_eeGeneral`, in `switch_driver.cpp`), so a file without `flexSwitches` leaves them unassigned |
| `int32 etx_write_radio_yaml(char*, int32)` | EdgeTX's writer (`writeGeneralSettings()`, with checksum) |
| `int32 etx_load_model_yaml(const char*, int32 len)` | 0 or an error: 1 empty, 2 nothing recognised, 3 a value overflows EdgeTX's parser (this EdgeTX's parser grows its buffer, so it does not happen), 4 storage. Parsed first into scratch space; then `readModel()` (EdgeTX's reader, with its pre-load defaults) into the live model and the data fix-ups of `postModelLoad()` (`loadCurves`, mix sort, legacy flags). A refused file leaves the live model as it was — including what a few EdgeTX readers write outside the structure they are given (colour screens and top bar, Lua user data, pre-3.0 function-switch keys), restored from the live model's own YAML. As on the radio, a file that stops parsing half-way is taken as far as it was read. **Runtime state is kept** (timers, latches, fades, slow) so a live edit does not reset the flight |
| `void etx_reset_runtime()` | what switching to a model resets: `flightReset`, special-function contexts, sticky logical switches, `restoreTimers`, function-switch start states, and the mixer state (slows, delays, outputs), with no flight-mode fade left running: like a power-on, the next tick starts in the flight mode the model's switches select, without a fade, and announces it (a model switch on the radio would instead fade from the flight mode the previous model was in, through the new model's mixes). What is not model state carries on: keys held (and their debounce), and the phase of the free-running 100 ms logical-switch tick (delays, durations, TIMER and EDGE count in it) |
| `int32 etx_new_model(int32 channelOrder)` | `setModelDefaults()`; `channelOrder` 0..23 overrides the radio's `templateSetup` (-1 keeps it); resets the runtime. The model is not given the virtual board's own owner ID (below) |
| `int32 etx_write_model_yaml(char*, int32)` | EdgeTX's model writer (`writeModelYaml()`), CRLF |
| `void etx_flush_model()` | `storageFlushCurrentModel()`: persistent timers, persistent sensors, pot warning positions |
| `void etx_flight_reset()` | *added* — the radio's "Reset flight" (quick menu, a RESET special function): `flightReset()` — timers that are not manual-reset start over, telemetry, logical switches, the throttle trace — then the pre-flight checks (a `checks` event). Not a model switch: persistent timers keep counting |
| `void etx_save_timers()` | *added* — `saveTimers()`: the persistent timers' running counts written into the model, as the radio does before it leaves a model (without the pot positions `etx_flush_model` also stores); `etx_model_changed()` then says whether one moved |
| `void etx_play_duration(int32 seconds, int32 flags)` | *added* — say `seconds` as the radio announces a duration (`playDuration()`): a `duration` event with its `parts`. `flags` 1 time of day, 2 long timer |
| `int32 etx_model_changed()` | 1 once after the radio changed the model itself (trims, GV adjust, instant trim, function switch states...); clears |
| `int32 etx_set_analog(int32 index, int32 value)` | calibrated position −1024..1024 of analog `index`, hardware order: sticks (LH, LV, RV, RH), then flex inputs (pots, sliders, 6POS, EXT). For a non-inverted input the mixer sees exactly `value` |
| `int32 etx_set_switch(int32 index, int32 position)` | hardware switch `index`: −1 up, 0 middle, +1 down (`simuSetSwitch` convention); a function switch (CFS) button is pressed when not up |
| `int32 etx_set_trim_key(int32 trim, int32 dir)` | hold trim button `trim` (hardware order T1 = LH, T2 = LV, T3 = RV, T4 = RH, T5...) in direction −1/+1, 0 releases; EdgeTX's key repeat and `checkTrims()` do the rest (step size, repeat, limits, flight mode, throttle trim, centre stop) |
| `int32 etx_set_key(int32 key, int32 pressed)` | any other key (`EnumKeys`) |
| `int32 etx_step(int32 ms)` | advance; returns ticks run |
| `uint32 etx_get_time()` | `g_tmr10ms` |
| `int32 etx_get_channels(int16*, int32)` | `channelOutputs`: −1024..1024 (±1536 with extended limits) |
| `int32 etx_get_pulses_us(uint16*, int32)` | PPM widths in **half microseconds**: `clamp(out, ±R) + 2 × (1500 + ppmCenter)` (`pulses/ppm.cpp`) |
| `int32 etx_get_mixer_outputs(int16*, int32)` | mixer outputs before limits (`ex_chans`) |
| `uint32 etx_get_used_channels()` | bit n: channel n+1 has mix lines (`isChannelUsed`) |
| `int32 etx_get_logical_switches(uint8*, int32)` | 0/1 per logical switch |
| `int32 etx_get_flight_mode()` | current flight mode |
| `int32 etx_get_gvar(int32 gv, int32 fm)` | effective GV value in `fm` (following links); `fm < 0`: current |
| `int32 etx_get_timer(int32 i, int32* out)` | value in seconds; `out` gets `[value, state, 10 ms counter]`, state 0 off, 1 running, 2 negative, 3 stopped |
| `int32 etx_get_trim(int32 i)` | trim `i` in the model's order (Rud, Ele, Thr, Ail, T5, T6), current flight mode — every trim the model stores, also those the radio has no buttons for (GX12: T5, T6) |
| `int32 etx_get_active_functions(uint8*, int32)` | per special function: bit 0 model function active, bit 1 radio (global) function active |
| `uint32 etx_get_function_flags()` | active function effects (`FUNCTION_*` bits) |
| `int32 etx_get_overrides(int16*, int32)` | OVERRIDE_CHANNEL value in percent per channel, −32768 when none |
| `int32 etx_get_analogs(int16*, int32)` | `calibratedAnalogs` used in the last tick |
| `int32 etx_get_active_lines(uint8*, int32)` | *added* — the radio's live highlighting of its Inputs and Mixes lists (`isExpoActive()`/`isMixActive()`, from the last mixer run): `MAX_EXPOS` bytes, one per input line (`expoData` index), bit 0 set when the line is the one driving its input (`mixState[].activeExpo`: the first line of the input whose switch, flight modes and side are on); then `MAX_MIXERS` bytes, one per mix line (`mixData` index), bit 0 set when the line contributed (`mixState[].activeMix`; a later REPLACE line un-marks the earlier lines of its channel). Lines the mixer never reaches read 0: those after the first input line without a mode, and on B&W radios those after the first mix line without a source (EdgeTX leaves their flags stale, or uninitialised). Returns the bytes written |
| `int32 etx_get_source_value(int32 source)` | *added* — `getValue(source)`: EdgeTX's current value of any mixer source (`describe().sources[].i`; negative = inverted) — −1024..1024 for sticks, pots, inputs, switches, channels, the raw value for GVs, timers and telemetry; 0 outside the source range. For a curve's crosshair and a logical switch's operands |
| `int32 etx_get_ls_sticky(uint8*, int32)` | *added* — per logical switch, 1 while a Sticky one is latched (`getLSStickyState()`, the radio's bold function label; it stays latched while an AND switch holds the result false), 0 for every other function |
| `int32 etx_check_throttle()` | *added* — 1 when the radio would stop at its throttle warning with the inputs as they are now (`isThrottleWarningAlertNeeded()`: 0 when the model disables it, against the custom idle position when it sets one, on the source the model traces). Set the inputs first |
| `int32 etx_check_switches(uint8*, int32)` | *added* — 1 when the radio would stop at its switch warning (`isSwitchWarningRequired()`). `out` gets one byte per switch in hardware order (0 where it is where the model wants it or has no warning, else the wanted position: 1 up, 2 middle, 3 down), then one per flex input (1 for a pot off the position the model's pot warning stored) |
| `int32 etx_eval_curve(const int8* points, int32 count, int32 custom, int32 smooth, const int16* xs, int16* ys, int32 n, int32 flags)` | *added* — EdgeTX's custom-curve maths (`applyCustomCurve()`: `intpol`, or `hermite_spline` when smooth) for a curve given by its points (as the model stores them: the Y values, then a custom curve's inner X): `ys[i]` for `xs[i]`. `flags` bit 0 xs in percent (`calc100toRESX`), bit 1 ys in percent (`calcRESXto100`), bit 2 mirrored (x negated); otherwise −1024..1024. The model's first curve slot is borrowed and restored. Returns `n`, or 0 for a curve EdgeTX does not store |
| `int32 etx_apply_curve_ref(int32 type, int32 value, const int16* xs, int16* ys, int32 n)` | *added* — `applyCurve()` for a Diff (0), Expo (1) or Function (2) reference with a constant value: `ys[i]` for `xs[i]`, −1024..1024. Returns `n` |
| `int32 etx_poll_events(char*, int32)` | drains the event log: JSON objects, one per line, whole lines only |
| `int32 etx_describe_json(char*, int32)` | the radio (below) |
| `int32 etx_schema_json(char*, int32)` | the model and radio YAML schemas (below) |

### Events

Each line is `{"t": <g_tmr10ms>, "ev": <kind>, ...}`:

| `ev` | fields | from |
|---|---|---|
| `sound` | `id` (EdgeTX `AU_*` index; PLAY_SOUND *n* is `AU_SPECIAL_SOUND_FIRST + n`, see `describe().enums.sounds`), `prompt` | system sounds, PLAY_SOUND, warnings |
| `tone` | `freq` (Hz), `len`, `pause` (ms), `flags`, `freqIncr` | what EdgeTX would synthesise for a sound under the radio's beep settings |
| `file` | `path`, `flags`, `id` | PLAY_TRACK, BACKGND_MUSIC, sounds with a file |
| `number` | `value`, `unit` (`TelemetryUnit`), `flags` (PREC1/PREC2...), `id` | PLAY_VALUE, telemetry, timers (everything spoken as a number) |
| `duration` | `seconds`, `flags`, `parts` | timer announcements, PLAY_VALUE of a timer. `parts` is what EdgeTX's English pack (`en_playDuration`) would play, in order: `{n, u, f}` per number (value, `TelemetryUnit`, flags) and `{p}` per prompt file (110 "and", 111 "minus") — so the host puts words to EdgeTX's decisions instead of repeating them |
| `countdown` | `timer`, `value` | timer countdown |
| `trim` | `value` | a trim step (the trim beep) |
| `model_audio` | `category`, `index`, `event` | per-model sound files the radio looks up (flight mode, switch, logical switch) |
| `haptic` | `id` | a haptic event, when the radio's haptic mode lets it through |
| `haptic_pattern` | `len`, `pause`, `flags`, `intensity` | explicit patterns |
| `volume` | `value` (0..23) | VOLUME function / radio volume source |
| `backlight` | `value` | BACKLIGHT function |
| `logs` | `on`, `period100ms` | SD LOGS function |
| `checks` | `boot` | flight reset → pre-flight checks |
| `warning`, `bubble` | `message`, `info` / `timeout` | pop-ups |
| `screenshot`, `screen` | `value` | SCREENSHOT, SET_SCREEN |
| `lua` | `file` | a Lua script the radio would run |
| `overflow` | | the first line after older events were dropped (256 KB kept); `t` is when |

### `etx_describe_json`

`api`, `edgetx {commit, version}`, `radio {target, flavour, lcd, colour, surface, stickMode,
channelOrder, channelOrders, throttleStick}`, `capacities` (outputs, mixes, expos, inputs,
logicalSwitches, specialFunctions, gvars, flightModes, curves, curvePoints, pointsPerCurve,
timers, trims, sticks, flexInputs, switches, allSwitches, flexSwitches, functionSwitches,
functionSwitchGroups, telemetrySensors, scripts, trainerChannels, multiposPositions), `limits`,
and:

- `analogs[]`: `index` (for `etx_set_analog`), `name` (hardware: LH, P1...), `label` (as the radio
  shows it; sticks by their current role), `type` (stick, pot, slider, multipos, switch, axis,
  none — from the radio's pot configuration), `config`/`defaultConfig` (radio.yml spelling),
  `inverted`, and the mixer `source` index and YAML `token` it feeds;
- `switches[]`: `index`, `name`, `hw` (2POS/3POS/ADC), `config` (radio type NONE/TOGGLE/2POS/3POS),
  `modelConfig` (effective, function-switch overrides applied), `default`, `settable`, `flex`,
  `cfs`, `cfsIndex`;
- `trims[]`: hardware trim `index`, `name`, the `modelTrim` it moves under the current stick mode,
  `label`, `token`; `keys[]`;
- `sources[]` — every mixer source index: `i`, `token` (exactly as model YAML writes it, from
  EdgeTX's encoder, and checked to read back to `i`; `null` where this radio cannot write it — a
  switch or pot it does not have, or GX12's `GR4`), `label` (English, as the radio shows it; arrows
  as Unicode), `icon` (stick, pot, slider, switch, trim, input, ...), `group`, `avail` (offered by
  the radio's source menus for the current radio settings and model);
- `switchSources[]` — every switch source: `i`, `token`, `label`, `group`, `avail` (bit 0 mixes,
  1 logical switches, 2 model special functions, 3 radio special functions);
- `enums`: `logicalSwitchFunctions`, `specialFunctions`, `mixMultiplex`, `timerModes`,
  `swashTypes` (value, YAML token, label), `curveRefTypes`, `curveFunctions`, `curveTypes`,
  `sounds`, `telemetryUnits` (*added*: `STR_VTELEMUNIT` by a sensor's `unit`, 0..`UNIT_MAX`; unit 0
  is shown without one).

The virtual board's owner ID. A radio.yml without `ownerRegistrationID` gets one from the board's
CPU id (`setDefaultOwnerId()`), the same on every install: nobody's radio. A model is therefore
not given it — neither a new model nor an imported one with a blank `modelRegistrationID` — so a
file made here takes the ID of the radio it is copied to, as a model Companion writes does. An ID
from the pilot's own radio.yml is given to models as the radio gives its own.

Negated sources and switches are written with a leading `!` (`"!SA2"`, `"!Rud"`).

### `etx_schema_json`

`{api, target, roots: {model, radio}, structs: {"<n>": [node...]}, enums: {"<n>": [{value,
token}...]}}` walked from the generated `YamlNode` trees
(`storage/yaml/yaml_datastructs_<radio>.cpp`). A node is `{tag, type, bits}` with, by type:
arrays `elmts`, `kind` (struct, keyed = written with an index key, list = written with `-`),
`struct` (a key of `structs`), `conditional`; unions `struct`, `selected`; enums `enum` (a key of
`enums`); strings `length`; `custom: true` where the value goes through a custom encoder (sources,
switches, special/logical function definitions...).

## JavaScript

In ArduConfigurator, `src/joystick/edgetx/engine.js` loads the module and wraps the C API:

```js
import { EdgeTxEngine } from "@/joystick/edgetx/engine.js";

const engine = await EdgeTxEngine.load(fetch("/edgetx/edgetx-tx16s.wasm"));
engine.loadModelYaml(text);          // throws EdgeTxEngineError with .code
engine.resetRuntime();
engine.setAnalog(1, 1024);           // Mode 2: LV (throttle) up
engine.step(16.67);                 // fractions of a ms are carried to the next step
engine.getPulsesUs();                // Float64Array, µs per channel
engine.pollEvents();                 // [{ t, ev, ... }]
if (engine.modelChanged()) save(engine.writeModelYaml());
engine.getActiveLines();             // { expos: Uint8Array, mixes: Uint8Array } | null
engine.getSourceValue(89);           // getValue(), or null on a module without it
```

The module is a WASI **preview1 reactor built without threads**: it needs no SharedArrayBuffer and
no cross-origin isolation, and runs wherever WebAssembly does (the production Node server sends no
COOP/COEP). The engine supplies the WASI imports itself (stdout/stderr → `engine.logs`, clock,
random; everything else `ENOSYS`).

## Licence

EdgeTX is licensed under the GNU GPL **version 2 only** (its files say "version 2 as published by
the Free Software Foundation", with no or-later clause). The source files in this directory are
GPL-2.0-or-later (SPDX headers), but a compiled `edgetx-<radio>.wasm` links EdgeTX, so the module
is a **GPL-2.0-only** program, which is what ArduConfigurator's `public/edgetx/manifest.json`
declares (`"license": "GPL-2.0-only"`, pinned by its `test/edgetxModules.test.js`).

**This public repository is the module's corresponding source.** A manifest names the commit each
module was built from (`edgetx.commit`, on branch `edgetx.branch` = `headless` of
`edgetx.repository` = <https://github.com/haltarkon/edgetx-wasm>, a fork of `edgetx.upstream`,
pinned in ArduConfigurator as `vendor/edgetx`). That one commit is all of it: EdgeTX with the
third-party submodules it pins, and this directory (whose digest the manifest also records as
`glue`); *Without ArduConfigurator* above is the complete build. ArduConfigurator builds and
ships the module separately from its own code — never bundled into the app's JavaScript, loaded at
run time like the app's SITL firmware.

The manifest also records what `test/edgetxModules.test.js` compares against ArduConfigurator's
tree, so a module cannot silently fall behind its sources: the commit, a SHA-256 over
`headless/src` and `headless/cmake` (`glue`), and the CMake options each radio was built with.
`--package` refuses an EdgeTX checkout with local changes, since the manifest could then only name
a commit the modules were not built from.

Nothing of EdgeTX is ported into ArduConfigurator's own source (its `src/`, GPL-3.0): where the
app needs EdgeTX's arithmetic (curves, what a duration announcement says), the module computes it
through the exports above. The app's `src/joystick/edgetx/NOTICE.md` records what it derives from
EdgeTX's behaviour and data.
