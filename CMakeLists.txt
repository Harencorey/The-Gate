cmake_minimum_required(VERSION 3.22)
if(NOT DEFINED TG_VERSION)
    set(TG_VERSION 0.1.0)   # CI passes 0.1.<build number>
endif()
project(TheGate VERSION ${TG_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)  # universal binary
endif()

option(TG_BUILD_TESTS "Build the offline engine tests" ON)

# JUCE: use a local checkout if you pass -DJUCE_DIR=/path/to/JUCE, otherwise download 8.0.15.
if(JUCE_DIR)
    add_subdirectory(${JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.15 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

if(APPLE)
    set(TG_FORMATS AU VST3 Standalone)
    set(TG_COPY_AFTER_BUILD TRUE)   # installs into ~/Library/Audio/Plug-Ins/
else()
    set(TG_FORMATS VST3 Standalone)
    set(TG_COPY_AFTER_BUILD FALSE)
endif()

juce_add_plugin(TheGate
    COMPANY_NAME "Norka DSP"
    BUNDLE_ID com.norkadsp.thegate
    PLUGIN_MANUFACTURER_CODE Nrka
    PLUGIN_CODE Gate              # keep this forever once it's been used in sessions
    FORMATS ${TG_FORMATS}
    PRODUCT_NAME "The Gate"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx Dynamics
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    # Standalone on macOS gets silence from the interface without this.
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "The Gate processes the live audio from your audio interface."
    COPY_PLUGIN_AFTER_BUILD ${TG_COPY_AFTER_BUILD})


target_sources(TheGate PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(TheGate PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(TheGate
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---- Tests --------------------------------------------------------------------
if(TG_BUILD_TESTS)
    enable_testing()
    # Engine tests: plain C++, no JUCE.
    add_executable(GateTests tests/test_engine.cpp)
    target_include_directories(GateTests PRIVATE Source)
    add_test(NAME engine COMMAND GateTests)

    # Plug-in level tests: the real processor, driven like a host drives it.
    juce_add_console_app(GateProcessorTests PRODUCT_NAME "GateProcessorTests")
    target_sources(GateProcessorTests PRIVATE
        tests/test_processor.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)
    target_include_directories(GateProcessorTests PRIVATE Source)
    target_compile_definitions(GateProcessorTests PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JucePlugin_Name="The Gate" JucePlugin_VersionString="${TG_VERSION}")
    target_link_libraries(GateProcessorTests PRIVATE
        juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
    add_test(NAME processor COMMAND GateProcessorTests)
endif()

# ---- Tools --------------------------------------------------------------------
option(TG_BUILD_TOOLS "Build the UI snapshot tool" OFF)
if(TG_BUILD_TOOLS)
    juce_add_console_app(GateSnapshot PRODUCT_NAME "GateSnapshot")
    target_sources(GateSnapshot PRIVATE tools/Snapshot.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
    target_include_directories(GateSnapshot PRIVATE Source)
    target_compile_definitions(GateSnapshot PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JucePlugin_Name="The Gate" JucePlugin_VersionString="${TG_VERSION}")
    target_link_libraries(GateSnapshot PRIVATE juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
endif()
