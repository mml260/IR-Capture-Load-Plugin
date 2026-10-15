cmake_minimum_required(VERSION 3.22)
project(IRCaptureLoad VERSION 0.1.0)

set(JUCE_DIR "" CACHE PATH "Path to the JUCE 8 source checkout")
if(NOT JUCE_DIR OR NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set JUCE_DIR to the root of a JUCE 8 checkout, for example -DJUCE_DIR=/path/to/JUCE")
endif()
add_subdirectory(${JUCE_DIR} JUCE)

juce_add_plugin(IRCaptureLoad
    COMPANY_NAME "IR Capture"
    BUNDLE_ID "com.ircapture.loader"
    PLUGIN_MANUFACTURER_CODE IRCp
    PLUGIN_CODE IRLd
    FORMATS AU
    PRODUCT_NAME "IR Capture + Load"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)

juce_generate_juce_header(IRCaptureLoad)
target_sources(IRCaptureLoad PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)
target_compile_features(IRCaptureLoad PRIVATE cxx_std_17)
target_compile_definitions(IRCaptureLoad PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)
target_link_libraries(IRCaptureLoad PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags
)
