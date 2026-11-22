# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

# Compiles the JUCE modules used by the DAW exactly once into a static library.
#
# JUCE module targets are INTERFACE libraries that inject their .cpp files into
# every consumer. Linking them PRIVATE here and re-exporting include dirs and
# compile definitions avoids duplicate symbols when several DAW modules use JUCE.
#
# Only non-GUI modules are used: Qt owns the UI. JUCE's MessageManager is still
# created (ScopedAudioRuntime) because AudioDeviceManager posts change messages;
# on Windows those are dispatched by Qt's native event loop.

add_library(daw_juce STATIC JuceSupport.cpp)
add_library(daw::juce ALIAS daw_juce)

target_link_libraries(daw_juce PRIVATE
    juce::juce_core
    juce::juce_events
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats)

target_compile_definitions(daw_juce PUBLIC
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    JUCE_STANDALONE_APPLICATION=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1
    JUCE_ASIO=$<BOOL:${DAW_ENABLE_ASIO}>)

if(DAW_ENABLE_ASIO)
    if(NOT EXISTS "${DAW_ASIO_SDK_DIR}/common/iasiodrv.h")
        message(FATAL_ERROR "DAW_ENABLE_ASIO=ON but DAW_ASIO_SDK_DIR does not contain common/iasiodrv.h")
    endif()
    target_include_directories(daw_juce PRIVATE "${DAW_ASIO_SDK_DIR}/common")
endif()

if(MSVC)
    target_compile_options(daw_juce PRIVATE /MP /bigobj /utf-8 /W0)
endif()

# Re-export the (transitively evaluated) include dirs and definitions of the
# JUCE modules to consumers, without re-exporting their sources.
target_include_directories(daw_juce SYSTEM INTERFACE $<TARGET_PROPERTY:daw_juce,INCLUDE_DIRECTORIES>)
target_compile_definitions(daw_juce INTERFACE $<TARGET_PROPERTY:daw_juce,COMPILE_DEFINITIONS>)

set_target_properties(daw_juce PROPERTIES FOLDER "third_party")
