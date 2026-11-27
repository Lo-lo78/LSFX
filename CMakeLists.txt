cmake_minimum_required(VERSION 3.22)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
project(LSFX VERSION 0.99.3 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(LSFX_EXPERIMENTAL_SF2_MULTIGROUP "Enable SF2 multi-output and channel compressors (validation build)" ON)
include(FetchContent)
FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.12 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)
# JUCE's VST3 client source is compiled later, during the build, not here.
# Apply also to an existing fetched checkout on a subsequent configure.
set(JUCE_SOURCE_DIR "${juce_SOURCE_DIR}")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchJuceProgramChange.cmake")
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
target_compile_definitions(LSFX PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=1)
target_link_libraries(LSFX PRIVATE sfizz::static FluidSynth::libfluidsynth juce::juce_audio_utils juce::juce_gui_basics PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

if(WIN32)
    target_compile_definitions(LSFX PRIVATE NOMINMAX)
    target_link_libraries(LSFX PRIVATE Uiautomationcore OleAut32)
endif()

# Standalone native smoke test; intentionally separate from the DAW process.
add_executable(LSFXFluidSmoke Tests/FluidSmoke.cpp)
target_link_libraries(LSFXFluidSmoke PRIVATE FluidSynth::libfluidsynth)
if(WIN32)
    target_compile_definitions(LSFXFluidSmoke PRIVATE NOMINMAX)
endif()

add_executable(LSFXFluidMapSmoke Tests/FluidMapSmoke.cpp)
target_link_libraries(LSFXFluidMapSmoke PRIVATE FluidSynth::libfluidsynth)
add_executable(LSFXVirtualProgramMapTest Tests/VirtualProgramMapTest.cpp)
if(WIN32)
    target_compile_definitions(LSFXFluidMapSmoke PRIVATE NOMINMAX)
endif()
# Keep the assertion checks active in Release CI.
if(MSVC)
    target_compile_options(LSFXVirtualProgramMapTest PRIVATE /UNDEBUG)
else()
    target_compile_options(LSFXVirtualProgramMapTest PRIVATE -UNDEBUG)
endif()

# Compile the same processor implementation into a separate console process.
# This exercises mixed playback/state without risking a DAW session.
juce_add_console_app(LSFXHybridSmoke PRODUCT_NAME "LSFXHybridSmoke")
juce_generate_juce_header(LSFXHybridSmoke)
target_sources(LSFXHybridSmoke PRIVATE Tests/HybridProcessorSmoke.cpp)
target_compile_definitions(LSFXHybridSmoke PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=1)
target_link_libraries(LSFXHybridSmoke PRIVATE sfizz::static FluidSynth::libfluidsynth juce::juce_audio_utils juce::juce_gui_basics juce::juce_recommended_config_flags)
if(WIN32)
    target_compile_definitions(LSFXHybridSmoke PRIVATE NOMINMAX)
    target_link_libraries(LSFXHybridSmoke PRIVATE Uiautomationcore OleAut32)
endif()

# TEST26.1 defaults to multigroup; configure OFF for the stereo recovery build.
if(LSFX_EXPERIMENTAL_SF2_MULTIGROUP)
    target_compile_definitions(LSFX PRIVATE LSFX_EXPERIMENTAL_SF2_MULTIGROUP=1)
    target_compile_definitions(LSFXHybridSmoke PRIVATE LSFX_EXPERIMENTAL_SF2_MULTIGROUP=1)
endif()
add_executable(LSFXChannelCompressorTest Tests/ChannelCompressorTest.cpp)
if(MSVC)
    target_compile_options(LSFXChannelCompressorTest PRIVATE /UNDEBUG)
else()
    target_compile_options(LSFXChannelCompressorTest PRIVATE -UNDEBUG)
endif()

# TEST27: validate the SF2 writer without the DAW or JUCE.
add_executable(LSFXSoundFontExportTest Tests/SoundFontExportTest.cpp)

add_executable(LSFXSfzSoundFontExportTest Tests/SfzSoundFontExportTest.cpp)

add_executable(LSFXFluidNoteRoutingSmoke Tests/FluidNoteRoutingSmoke.cpp)
target_link_libraries(LSFXFluidNoteRoutingSmoke PRIVATE FluidSynth::libfluidsynth)

add_executable(LSFXNoteRouteGroupsTest Tests/NoteRouteGroupsTest.cpp)
if(MSVC)
    target_compile_options(LSFXNoteRouteGroupsTest PRIVATE /UNDEBUG)
else()
    target_compile_options(LSFXNoteRouteGroupsTest PRIVATE -UNDEBUG)
endif()

# TEST32: keyboard-grid and channel digit navigation, independent of JUCE.
add_executable(LSFXGridNavigationTest Tests/GridNavigationTest.cpp)
if(MSVC)
    target_compile_options(LSFXGridNavigationTest PRIVATE /UNDEBUG)
else()
    target_compile_options(LSFXGridNavigationTest PRIVATE -UNDEBUG)
endif()
