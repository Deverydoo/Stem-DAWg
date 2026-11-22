# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

daw_add_module(core
    SOURCES
        src/Result.cpp
        src/Ids.cpp
        src/RtThread.cpp
        src/RtEvent.cpp
        src/Files.cpp
        src/Logger.cpp
        src/AudioAsset.cpp
    PUBLIC_DEPS
        nlohmann_json::nlohmann_json)
