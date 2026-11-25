cmake_minimum_required(VERSION 3.22)
project(LSFX VERSION 0.0.1 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)
FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.12 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)
find_package(FluidSynth CONFIG REQUIRED)
set(SFIZZ_SHARED OFF CACHE BOOL "" FORCE)
set(SFIZZ_JACK OFF CACHE BOOL "" FORCE)
set(SFIZZ_RENDER OFF CACHE BOOL "" FORCE)
set(ENABLE_LTO OFF CACHE BOOL "" FORCE)
FetchContent_Declare(sfizz GIT_REPOSITORY https://github.com/sfztools/sfizz.git GIT_TAG 1.2.3 GIT_SHALLOW TRUE GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(sfizz)
juce_add_plugin(LSFX
    COMPANY_NAME "Lo-lo" BUNDLE_ID "org.lolo.lsoundfont"
    PLUGIN_MANUFACTURER_CODE Lolo PLUGIN_CODE Lsft
    FORMATS VST3 PRODUCT_NAME "LSFX"
    IS_SYNTH TRUE NEEDS_MIDI_INPUT TRUE NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE VST3_CATEGORIES Instrument Synth)
juce_generate_juce_header(LSFX)
target_sources(LSFX PRIVATE Source/Plugin.cpp)
target_compile_definitions(LSFX PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(LSFX PRIVATE sfizz::static FluidSynth::libfluidsynth juce::juce_audio_utils juce::juce_gui_basics PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
