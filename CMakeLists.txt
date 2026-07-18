# BASAMAK - a free, open-source drum & sample step-sequencer plugin (VST3 / AU / Standalone).
# Copyright (C) 2026 Oguzhan Yazici. Licensed under the GNU AGPL v3 (see LICENSE).
cmake_minimum_required(VERSION 3.22)
project(BASAMAK VERSION 1.5.0)   # [2026-07-18] 1.5.0 = PER-BAR play modes in merged groups (the group playback override is gone)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows: statically link the MSVC runtime into the binary so users DON'T need the
# Visual C++ Redistributable installed - the plugin/standalone just runs as-is.
# (CMP0091 is NEW by default on CMake >= 3.15, so CMAKE_MSVC_RUNTIME_LIBRARY is honored.)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Point to JUCE. Resolution order: -DJUCE_DIR=..., then a JUCE_DIR env var,
# then ~/JUCE on macOS/Linux, then %USERPROFILE%\JUCE on Windows.
if(NOT DEFINED JUCE_DIR)
    if(DEFINED ENV{JUCE_DIR})
        set(JUCE_DIR "$ENV{JUCE_DIR}")
    elseif(EXISTS "$ENV{HOME}/JUCE/CMakeLists.txt")
        set(JUCE_DIR "$ENV{HOME}/JUCE")
    elseif(EXISTS "$ENV{USERPROFILE}/JUCE/CMakeLists.txt")
        set(JUCE_DIR "$ENV{USERPROFILE}/JUCE")
    endif()
endif()
if(NOT JUCE_DIR OR NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found. Clone it and pass -DJUCE_DIR=path/to/JUCE "
                        "(get JUCE: git clone https://github.com/juce-framework/JUCE)")
endif()
add_subdirectory(${JUCE_DIR} JUCE)

string(TIMESTAMP BUILD_TS "%Y%m%d-%H%M")

# Dev builds embed a timestamp in the product name so iterative rebuilds never
# leave two installed copies sharing the same plugin UID (which hangs Reaper's
# scanner). For a distributable build pass -DDAVULSEQ_RELEASE=ON to get a clean
# "DavulSEQ" name with no timestamp and no auto-install.
option(DAVULSEQ_RELEASE "Clean product name for distribution, no auto-install" OFF)
if(DAVULSEQ_RELEASE)
    set(DAVULSEQ_PRODUCT_NAME "BASAMAK")
    set(DAVULSEQ_COPY_AFTER_BUILD FALSE)
else()
    set(DAVULSEQ_PRODUCT_NAME "BASAMAK ${BUILD_TS}")
    set(DAVULSEQ_COPY_AFTER_BUILD TRUE)
endif()

juce_add_plugin(BASAMAK
    # [2026-07-15 19:45] app icon (the wordmark's "B" glyph, white on the plugin's canvas navy):
    # embedded into the Windows .exe, the macOS .app/.component/.vst3 icns, and the Linux window icon.
    ICON_BIG   "Resources/Icon/basamak-icon-512.png"
    ICON_SMALL "Resources/Icon/basamak-icon-128.png"
    COMPANY_NAME "OZ"
    VERSION ${PROJECT_VERSION}
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT FALSE
    # Without a Bluetooth usage description in Info.plist, macOS kills the app the
    # moment it touches the Bluetooth API - which is exactly what the standalone's
    # "Bluetooth" (BLE MIDI) button does. This adds NSBluetoothAlwaysUsageDescription.
    BLUETOOTH_PERMISSION_ENABLED TRUE
    BLUETOOTH_PERMISSION_TEXT "BASAMAK uses Bluetooth to connect Bluetooth MIDI controllers."
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD ${DAVULSEQ_COPY_AFTER_BUILD}
    PLUGIN_MANUFACTURER_CODE OZ95
    PLUGIN_CODE Bsmk
    # BUNDLE_ID pinned to the pre-rename value: hosts/macOS already know this identity, and the
    # plugin's REAL identity (manufacturer/plugin codes above) never changes anyway.
    BUNDLE_ID com.OZ.DrumSequencer
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "${DAVULSEQ_PRODUCT_NAME}"
)

juce_generate_juce_header(BASAMAK)

# ---- Offline DSP regression tests (tests/run.sh builds + runs them all) ----
# Console apps rendering the engine headlessly with Goertzel checks; OFF by default so normal
# builds are untouched. Run tests/run.sh BEFORE EVERY RELEASE - each test exits non-zero on FAIL.
option(BASAMAK_TESTS "Build the offline DSP regression tests" OFF)
if(BASAMAK_TESTS)
    foreach(T PolyTest PRollTest MergeTest PresetTest DuckTest NudgeTest SlotNoteTest GlideTest ArpTest TunerTest GateWrapTest LiveRollTest RollAbsoluteTest WavetableTest AliveTest GrainTest ModMatrixTest WaveguideTest BankAudit UIAudit MixDump)
        juce_add_console_app(${T})
        juce_generate_juce_header(${T})
        target_sources(${T} PRIVATE
            Source/dsp/DrumChannel.cpp
            Source/dsp/Sequencer.cpp
            Source/plugin/FactoryContent.cpp
            tests/${T}.cpp)
        target_include_directories(${T} PRIVATE Source/dsp Source/midi Source/ui Source/plugin)
        target_compile_definitions(${T} PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 BASAMAK_VERSION="test")
        target_link_libraries(${T} PRIVATE juce::juce_audio_utils juce::juce_dsp juce::juce_gui_extra
            PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags)
    endforeach()
endif()




target_sources(BASAMAK PRIVATE
    Source/plugin/PluginProcessor.cpp
    Source/plugin/FactoryContent.cpp
    Source/ui/PluginEditor.cpp
    Source/ui/StepGridComponent.cpp
    Source/dsp/DrumChannel.cpp
    Source/dsp/Sequencer.cpp
    Source/midi/MidiLearnManager.cpp
)

# Add each source folder to the include path so the plain #include "X.h"
# lines keep resolving regardless of which subfolder a file lives in.
target_include_directories(BASAMAK PRIVATE
    Source/dsp
    Source/midi
    Source/ui
    Source/plugin
)

target_compile_definitions(BASAMAK PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    BASAMAK_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(BASAMAK PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_plugin_client
    juce::juce_dsp
    juce::juce_gui_extra
PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# ---- Optional SoundTouch (time-stretch). Two ways to enable: ----
#   (a) Homebrew:  brew install sound-touch   (arm64 lib -> build arm64-only)
#   (b) Source:    git clone https://codeberg.org/soundtouch/soundtouch external/soundtouch  (universal)
# Without either, the plugin builds fine and the Stretch control is a no-op (BASAMAK_HAVE_SOUNDTOUCH unset).
set(ST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/soundtouch")
if(EXISTS "${ST_DIR}/include/SoundTouch.h")
    file(GLOB ST_SRC "${ST_DIR}/source/SoundTouch/*.cpp")
    target_sources(BASAMAK PRIVATE ${ST_SRC})
    target_include_directories(BASAMAK PRIVATE "${ST_DIR}/include")
    target_compile_definitions(BASAMAK PUBLIC BASAMAK_HAVE_SOUNDTOUCH=1)
    message(STATUS "BASAMAK: SoundTouch (source) -> time-stretch ENABLED")
else()
    find_path(ST_INC SoundTouch.h PATHS /opt/homebrew/include/soundtouch /usr/local/include/soundtouch)
    find_library(ST_LIB NAMES SoundTouch PATHS /opt/homebrew/lib /usr/local/lib)
    if(ST_INC AND ST_LIB)
        target_include_directories(BASAMAK PRIVATE "${ST_INC}")
        target_link_libraries(BASAMAK PRIVATE "${ST_LIB}")
        target_compile_definitions(BASAMAK PUBLIC BASAMAK_HAVE_SOUNDTOUCH=1)
        message(STATUS "BASAMAK: SoundTouch (Homebrew: ${ST_LIB}) -> time-stretch ENABLED")
    else()
        message(STATUS "BASAMAK: SoundTouch NOT found -> time-stretch disabled")
    endif()
endif()
