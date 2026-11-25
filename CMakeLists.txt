cmake_minimum_required(VERSION 3.22)
project(LSoundFont VERSION 0.0.1 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)
FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.12 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)
find_package(FluidSynth CONFIG REQUIRED)
juce_add_plugin(LSoundFont
    COMPANY_NAME "Lo-lo" BUNDLE_ID "org.lolo.lsoundfont"
    PLUGIN_MANUFACTURER_CODE Lolo PLUGIN_CODE Lsft
    FORMATS VST3 PRODUCT_NAME "LSoundFont"
    IS_SYNTH TRUE NEEDS_MIDI_INPUT TRUE NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE VST3_CATEGORIES Instrument Synth)
juce_generate_juce_header(LSoundFont)
target_sources(LSoundFont PRIVATE Source/Plugin.cpp)
target_compile_definitions(LSoundFont PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(LSoundFont PRIVATE FluidSynth::libfluidsynth juce::juce_audio_utils juce::juce_gui_basics PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
