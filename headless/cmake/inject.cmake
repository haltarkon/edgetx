# SPDX-License-Identifier: GPL-2.0-or-later
#
# Injected into EdgeTX's own CMake project (the repository this directory is in) through
#   -DCMAKE_PROJECT_EdgeTX_INCLUDE=<this file>
# so that the headless mixer is built by EdgeTX's build, with EdgeTX's
# per-radio configuration, without a single line of EdgeTX being edited.
#
# project(EdgeTX) includes this file right after it runs. At that point none of
# EdgeTX's targets exist yet, so all we do here is schedule a call for the end
# of the top-level directory (cmake_language DEFER). By then radio/src has
# declared `radiolib_native` (every radio source, as an OBJECT library, with the
# radio's definitions) and `simu_drivers` (the simulator's board stubs), and we
#
#   1. retarget them to a single-threaded port: drop NATIVE_THREADS and the
#      os/*_native.cpp std::thread implementation, and force-include our port
#      header, which supplies the task/mutex/timer handle types EdgeTX's os/
#      headers leave to the port;
#   2. drop the simulator drivers that export the simulator's own WASM API or
#      reach for the host filesystem (our glue is the board support instead);
#   3. add our glue sources to radiolib_native itself, so they are compiled with
#      exactly the definitions, include paths and flags of the radio code whose
#      structures they share;
#   4. archive all of it and link `edgetx-headless`, a WASI reactor, from that
#      archive: only the members our API reaches are pulled in.

if(NOT DEFINED ETX_WASM_DIR)
  get_filename_component(ETX_WASM_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(ETX_API_VERSION "1" CACHE STRING "edgetx-headless C API version")

# The virtual radio: a radio that exists only as this module. It is configured
# as a real one (the options name it: -DPCB=X10 -DPCBREV=TX16S), so that every
# size and every model field is one EdgeTX ships, and differs in its hardware:
# two gimbals with their trims, and instead of pots and switches the host's own
# controls -- controller axes and buttons, a mouse, keyboard keys -- as mixer
# sources and switches (HOST_INPUTS, radio/src/hal/host_inputs.h; the tables
# are src/etx_board.cpp and src/etx_host_inputs.cpp). The module is named
# edgetx-virtual.wasm.
option(ETX_VIRTUAL_RADIO "Build the virtual radio (host inputs) instead of the configured one" OFF)

function(_etx_filter_options out_var)
  # Remove the options selecting the std::thread port; keep everything else in order.
  set(result "")
  foreach(opt IN LISTS ARGN)
    if(NOT opt STREQUAL "-DNATIVE_THREADS")
      list(APPEND result "${opt}")
    endif()
  endforeach()
  set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

function(_etx_add_headless_target)
  if(NOT TARGET radiolib_native OR NOT TARGET simu_drivers)
    message(FATAL_ERROR "edgetx-headless: EdgeTX did not declare radiolib_native/simu_drivers "
                        "(configure with -DNATIVE_BUILD=ON -DEdgeTX_SUPERBUILD=OFF)")
  endif()
  if(NOT WASI)
    message(FATAL_ERROR "edgetx-headless: configure with the WASI SDK toolchain")
  endif()

  set(port_header "${ETX_WASM_DIR}/src/etx_port.h")

  # FLAVOUR (tx16s, gx12, ...) is set by radio/src's target CMake, in that scope.
  get_directory_property(ETX_TARGET_NAME DIRECTORY "${RADIO_SRC_DIR}" DEFINITION FLAVOUR)
  if(NOT ETX_TARGET_NAME)
    message(FATAL_ERROR "edgetx-headless: radio/src did not set FLAVOUR")
  endif()
  # The radio EdgeTX is configured as; the virtual radio is named after itself.
  set(ETX_BASE_NAME "${ETX_TARGET_NAME}")
  if(ETX_VIRTUAL_RADIO)
    set(ETX_TARGET_NAME "virtual")
  endif()

  # --- 1. radiolib_native -> single-threaded port -------------------------------
  get_target_property(srcs radiolib_native SOURCES)
  list(FILTER srcs EXCLUDE REGEX "(^|/)os/[a-z_]+_native\\.cpp$")
  get_directory_property(colorlcd DIRECTORY "${RADIO_SRC_DIR}" DEFINITION GUI_DIR)
  if(colorlcd STREQUAL "colorlcd")
    # The colour GUI (LVGL, its fonts, the pages, Lua's LVGL bindings) is left
    # out of the link altogether. Pages register themselves from static
    # constructors, and wasm-ld keeps the constructors of every object that
    # provides one live symbol -- even an inline std::string member it happened
    # to take from a GUI object -- so linking the GUI at all drags most of it
    # in. Nothing on the mixer's path needs it; the few entry points that
    # would reach it are wrapped (see ETX_WRAP_SYMBOLS), and the build fails if
    # anything left undefined is reachable (--import-undefined + import check).
    # Kept: the three files that implement the model's screen/top-bar/widget
    # data classes, which the model YAML reader and writer use.
    set(kept_gui "")
    foreach(src IN LISTS srcs)
      if(src MATCHES "gui/colorlcd/mainview/(layout|topbar|widget)\\.cpp$")
        list(APPEND kept_gui "${src}")
      endif()
    endforeach()
    list(FILTER srcs EXCLUDE REGEX "(^|/)gui/colorlcd/|(^|/)fonts/lvgl/|(^|/)thirdparty/lvgl/|(^|/)lua/(lua_lvgl|api_colorlcd)")
    list(APPEND srcs ${kept_gui})
    set(ETX_GUI_EXCLUDED ON)
  endif()
  set_property(TARGET radiolib_native PROPERTY SOURCES ${srcs})
  set_property(TARGET radiolib_native PROPERTY INTERFACE_SOURCES "")

  foreach(tgt radiolib_native simu_drivers)
    foreach(prop COMPILE_OPTIONS INTERFACE_COMPILE_OPTIONS)
      get_target_property(opts ${tgt} ${prop})
      if(opts)
        _etx_filter_options(opts ${opts})
        set_property(TARGET ${tgt} PROPERTY ${prop} ${opts})
      endif()
    endforeach()
    target_compile_options(${tgt} PRIVATE "SHELL:-include ${port_header}")
    target_compile_definitions(${tgt} PRIVATE ETX_HEADLESS=1)
    if(ETX_VIRTUAL_RADIO)
      target_compile_definitions(${tgt} PRIVATE ETX_VIRTUAL_RADIO=1 HOST_INPUTS=1)
      # FLAVOUR is what EdgeTX writes as a radio.yml's `board:` (and into its
      # version strings): this radio's settings are not a TX16S's. radio/src
      # defines it for the directory; options follow definitions on the
      # command line, so the redefinition here is the one that holds.
      target_compile_options(${tgt} PRIVATE -UFLAVOUR "-DFLAVOUR=\"virtual\"")
    endif()
  endforeach()

  # --- 2. simulator drivers: keep only the plain stubs --------------------------
  get_target_property(simu_srcs simu_drivers SOURCES)
  get_target_property(simu_dir simu_drivers SOURCE_DIR)
  set(kept "")
  foreach(src IN LISTS simu_srcs)
    get_filename_component(name "${src}" NAME)
    # simulib/audio/adc/switch export the simulator's WASM API (export_name
    # attributes would make them link roots); simufatfs/simudisk use the host
    # filesystem. etx_board.cpp and etx_fs.cpp stand in for them.
    if(NOT name MATCHES "^(simulib|simufatfs|simudisk|audio_driver|adc_driver|switch_driver)\\.cpp$")
      list(APPEND kept "${src}")
    endif()
  endforeach()
  set_property(TARGET simu_drivers PROPERTY SOURCES ${kept})

  # --- 3. our glue, compiled as radio code --------------------------------------
  target_sources(radiolib_native PRIVATE
    ${ETX_WASM_DIR}/src/etx_port.cpp
    ${ETX_WASM_DIR}/src/etx_fs.cpp
    ${ETX_WASM_DIR}/src/etx_board.cpp
    ${ETX_WASM_DIR}/src/etx_host_inputs.cpp
    ${ETX_WASM_DIR}/src/etx_api.cpp
    ${ETX_WASM_DIR}/src/etx_describe.cpp
  )
  set_property(SOURCE ${ETX_WASM_DIR}/src/etx_api.cpp ${ETX_WASM_DIR}/src/etx_describe.cpp
    TARGET_DIRECTORY radiolib_native
    APPEND PROPERTY COMPILE_DEFINITIONS
      ETX_API_VERSION=${ETX_API_VERSION}
      ETX_EDGETX_COMMIT="${ETX_EDGETX_COMMIT}"
      ETX_TARGET_NAME="${ETX_TARGET_NAME}"
      ETX_BASE_NAME="${ETX_BASE_NAME}")
  # simu_switches.inc & friends are generated next to radio/src's binary dir
  target_include_directories(radiolib_native PRIVATE ${simu_dir})

  # --- 4. archive + reactor -----------------------------------------------------
  add_library(etx_radio STATIC EXCLUDE_FROM_ALL
    $<TARGET_OBJECTS:radiolib_native>
    $<TARGET_OBJECTS:simu_drivers>)
  set_target_properties(etx_radio PROPERTIES LINKER_LANGUAGE CXX)

  add_executable(edgetx-headless ${ETX_WASM_DIR}/src/etx_link.cpp)
  set_target_properties(edgetx-headless PROPERTIES
    OUTPUT_NAME "edgetx-${ETX_TARGET_NAME}"
    SUFFIX ".wasm")
  target_link_libraries(edgetx-headless PRIVATE etx_radio)

  # Link-time substitutions (see "Link-time wrappers" in src/etx_api.cpp):
  # sound and haptic requests become events, and the few UI entry points the
  # mixer can reach (pre-flight checks, screenshots, screen switching) report
  # instead of drawing or waiting. Itanium-mangled names of EdgeTX functions.
  set(ETX_WRAP_SYMBOLS
    _Z10audioEventj                  # audioEvent(unsigned)
    _Z9audioPlayjh                   # audioPlay(unsigned, uint8_t)
    _Z14audioTrimPressi              # audioTrimPress(int)
    _Z19audioTimerCountdownhi        # audioTimerCountdown(uint8_t, int)
    _Z14playModelEventhht            # playModelEvent(uint8_t, uint8_t, event_t)
    _ZN10AudioQueue8playFileEPKchha  # AudioQueue::playFile(...)
    _ZN10AudioQueue8playToneEttthaa  # AudioQueue::playTone(...)
    _ZN11hapticQueue5eventEh         # hapticQueue::event(uint8_t)
    _ZN11hapticQueue4playEhhhh       # hapticQueue::play(...)
    _Z8checkAllb                     # checkAll(bool)
    _Z13POPUP_WARNINGPKcS0_          # POPUP_WARNING(const char*, const char*)
    _Z24POPUP_WARNING_ON_UI_TASKPKcS0_ # POPUP_WARNING_ON_UI_TASK(...)
    _Z15writeScreenshotv             # writeScreenshot()
    _Z20setRequestedMainViewh        # setRequestedMainView(uint8_t)   colour
    _Z15showTelemScreenh             # showTelemScreen(uint8_t)        B&W
    _Z7luaExecPKc                    # luaExec(const char*)
    _Z12POPUP_BUBBLEPKcjii           # POPUP_BUBBLE(...)               colour
    _ZN13LayoutFactory19deleteCustomScreensEv  # LayoutFactory::... colour
    _ZN13LayoutFactory19deleteTopBarWidgetsEv  # LayoutFactory::... colour
    _ZNK8ViewMain18getCurrentMainViewEv        # ViewMain::...       colour
    _ZN8ViewMain8instanceEv                    # ViewMain::instance() colour
    _Z12menuMainViewt                # menuMainView(event_t)           B&W
    _Z17menuViewTelemetryt           # menuViewTelemetry(event_t)      B&W
    _Z16menuChannelsViewt            # menuChannelsView(event_t)       B&W
  )
  set(wraps "")
  foreach(sym IN LISTS ETX_WRAP_SYMBOLS)
    list(APPEND wraps "-Wl,--wrap=${sym}")
  endforeach()

  target_link_options(edgetx-headless PRIVATE
    -mexec-model=reactor
    -Wl,--gc-sections
    -Wl,--strip-all
    # etx_api.o carries the export_name attributes of the whole API; pulling it
    # out of the archive is all it takes to export them.
    -Wl,--undefined=etx_api_version
    -Wl,--export=malloc -Wl,--export=free
    -Wl,-z,stack-size=${ETX_STACK_SIZE}
    -Wl,--max-memory=${ETX_MAX_MEMORY}
    -Wl,-mllvm,-wasm-enable-sjlj
    $<$<BOOL:${ETX_GUI_EXCLUDED}>:-Wl,--import-undefined>
    -lsetjmp
    ${wraps}
  )
endfunction()

if(NOT DEFINED ETX_STACK_SIZE)
  set(ETX_STACK_SIZE 262144)
endif()
if(NOT DEFINED ETX_MAX_MEMORY)
  set(ETX_MAX_MEMORY 67108864)
endif()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _etx_add_headless_target)
