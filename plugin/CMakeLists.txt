cmake_minimum_required(VERSION 3.22)

# Default to Release unless the build directory was explicitly configured
# otherwise: this is a live-use plugin, also rendered in offline bounces, not
# something run under a debugger day to day. Only takes effect on a *fresh*
# configure -- CMAKE_BUILD_TYPE is cached per build directory.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# Without this, CMake defaults the deployment target to whatever macOS the
# build machine happens to run, which quietly produces a plugin that refuses
# to load on anything older. Released builds target macOS 13.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

# Universal (Apple Silicon + Intel). Live rigs running MainStage still include
# Intel Macs, and an Intel-only or Rosetta-hosted AU runs out of process in
# Logic/MainStage, which the instance link must not depend on. Override with
# -DCMAKE_OSX_ARCHITECTURES=arm64 for a faster local build.
if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" FORCE)
endif()

project(SGTMAutomix VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE is missing. It is a git submodule: run `git submodule update --init` "
        "from the repository root (see BUILDING.md).")
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../JUCE JUCE)

# --------------------------------------------------------------------------------------------------
# Formats. AU is macOS-only. AAX builds on macOS and Windows from the AAX SDK
# that ships inside JUCE; an unsigned AAX only loads in Pro Tools Developer
# (see BUILDING.md).

set(SGTM_AAX_DEFAULT OFF)
if(APPLE OR WIN32)
    set(SGTM_AAX_DEFAULT ON)
endif()
option(SGTM_BUILD_AAX "Build the AAX format (macOS/Windows only)" ${SGTM_AAX_DEFAULT})
option(SGTM_BUILD_PROBE "Build the SGTM Host Probe development plugin" ON)

set(SGTM_FORMATS VST3)
if(APPLE)
    list(APPEND SGTM_FORMATS AU)
endif()
if(SGTM_BUILD_AAX AND (APPLE OR WIN32))
    list(APPEND SGTM_FORMATS AAX)
endif()
message(STATUS "SGTM Automix: building ${SGTM_FORMATS}")

# The SGTM wordmark, compiled in as Assets::sgtm_logo_png. Underscores in the
# filename are load-bearing: juce_add_binary_data's identifier generator
# deletes hyphens rather than converting them.
juce_add_binary_data(SGTMAutomixAssets NAMESPACE Assets SOURCES Resources/sgtm_logo.png)

# --------------------------------------------------------------------------------------------------
# SGTM Automix

juce_add_plugin(SGTMAutomix
    COMPANY_NAME "SGTM"
    BUNDLE_ID com.sgtm.Automix
    PLUGIN_MANUFACTURER_CODE Sgtm
    PLUGIN_CODE Smix
    FORMATS ${SGTM_FORMATS}
    PRODUCT_NAME "SGTM Automix"
    DESCRIPTION "Gain-sharing automixer for speech"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Fx Dynamics
    AU_MAIN_TYPE kAudioUnitType_Effect
    AAX_CATEGORY AAX_ePlugInCategory_Dynamics
    COPY_PLUGIN_AFTER_BUILD TRUE
)

target_sources(SGTMAutomix
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
)

target_compile_definitions(SGTMAutomix
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(SGTMAutomix
    PRIVATE
        SGTMAutomixAssets
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# --------------------------------------------------------------------------------------------------
# SGTM Host Probe: development tool that logs how a host schedules processBlock
# calls per instance. Not part of a release.

if(SGTM_BUILD_PROBE)
    juce_add_plugin(SGTMHostProbe
        COMPANY_NAME "SGTM"
        BUNDLE_ID com.sgtm.HostProbe
        PLUGIN_MANUFACTURER_CODE Sgtm
        PLUGIN_CODE Sprb
        FORMATS ${SGTM_FORMATS}
        PRODUCT_NAME "SGTM Host Probe"
        DESCRIPTION "Logs host processBlock timing (development tool)"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        VST3_CATEGORIES Fx Tools
        AU_MAIN_TYPE kAudioUnitType_Effect
        AAX_CATEGORY AAX_ePlugInCategory_None
        COPY_PLUGIN_AFTER_BUILD TRUE
    )

    target_sources(SGTMHostProbe PRIVATE Source/HostProbe.cpp)

    target_compile_definitions(SGTMHostProbe
        PUBLIC
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_VST3_CAN_REPLACE_VST2=0
    )

    target_link_libraries(SGTMHostProbe
        PRIVATE
            juce::juce_audio_utils
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )
endif()

# --------------------------------------------------------------------------------------------------
# Standalone harness that checks the automix engine and the instance link
# directly (gain shares for equal and unequal channels, pass-through,
# block-size independence, several linked instances live and offline). No JUCE.

add_executable(SGTMAutomixVerify Source/verify_main.cpp)

find_package(Threads REQUIRED)
target_link_libraries(SGTMAutomixVerify PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
    # shm_open lives in librt on older glibc.
    target_link_libraries(SGTMAutomixVerify PRIVATE rt)
    target_link_libraries(SGTMAutomix PRIVATE rt)
endif()

# Loads the built VST3 bundles the way a host does and checks latency,
# pass-through, parameters, state, editor and probe logging.
juce_add_console_app(SGTMAutomixSmokeTest PRODUCT_NAME "SGTM Automix Smoke Test")

target_sources(SGTMAutomixSmokeTest PRIVATE Source/smoke_test_main.cpp)

target_compile_definitions(SGTMAutomixSmokeTest
    PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

target_link_libraries(SGTMAutomixSmokeTest
    PRIVATE
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
