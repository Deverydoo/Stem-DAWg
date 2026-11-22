# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

daw_add_module(engine
    SOURCES
        src/AudioEngine.cpp
        src/JuceAudioDeviceService.cpp
        src/AudioRuntime.cpp
    PUBLIC_DEPS
        daw::core
    PRIVATE_DEPS
        daw::juce)
