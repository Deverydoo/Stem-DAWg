# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

daw_add_module(session
    SOURCES
        src/MediaPool.cpp
        src/MediaLoader.cpp
        src/ProjectOperations.cpp
        src/SnapshotBuilder.cpp
        src/Renderer.cpp
        src/StemReport.cpp
        src/UndoStack.cpp
        src/NoiseTools.cpp
        src/VoiceTools.cpp
    PUBLIC_DEPS
        daw::core
        daw::media
        daw::engine
        daw::project
        daw::dsp
        daw::ai)
