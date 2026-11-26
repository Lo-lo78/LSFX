cmake_minimum_required(VERSION 3.22)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
project(LSFX VERSION 0.0.1 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)
FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.12 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)
find_package(FluidSynth CONFIG REQUIRED)
set(ABSL_MSVC_STATIC_RUNTIME OFF CACHE BOOL "Use dynamic MSVC runtime for Abseil" FORCE)
set(SFIZZ_SHARED OFF CACHE BOOL "" FORCE)
set(SFIZZ_JACK OFF CACHE BOOL "" FORCE)
set(SFIZZ_RENDER OFF CACHE BOOL "" FORCE)
set(ENABLE_LTO OFF CACHE BOOL "" FORCE)
FetchContent_Declare(sfizz GIT_REPOSITORY https://github.com/sfztools/sfizz.git GIT_TAG 1.2.3 GIT_SHALLOW TRUE GIT_SUBMODULES_RECURSE TRUE
    PATCH_COMMAND "${CMAKE_COMMAND}" "-DSFIZZ_SOURCE_DIR=<SOURCE_DIR>" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchSfizz.cmake")
FetchContent_MakeAvailable(sfizz)
# Also normalize the actual targets, including nested bundled dependencies,
# in case a vendor overrides the inherited runtime setting.
if(MSVC)
    function(lsfx_use_dynamic_crt directory)
        get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(t IN LISTS targets)
            get_target_property(kind "${t}" TYPE)
            if(kind MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
                set_property(TARGET "${t}" PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
            endif()
        endforeach()
        get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
        foreach(child IN LISTS children)
            lsfx_use_dynamic_crt("${child}")
        endforeach()
    endfunction()
    lsfx_use_dynamic_crt("${sfizz_SOURCE_DIR}")
    message(STATUS "LSFX: normalized sfizz and bundled targets to the /MD CRT")
endif()
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

if(WIN32)
    target_link_libraries(LSFX PRIVATE Uiautomationcore OleAut32)
endif()
