# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

daw_add_module(media
    SOURCES
        src/AudioFileDecoder.cpp
        src/WavWriter.cpp
        src/AudioTags.cpp
        src/Resampler.cpp
        src/PeakPyramid.cpp
        src/PeakCache.cpp
    PUBLIC_DEPS
        daw::core
    PRIVATE_DEPS
        daw::juce)
