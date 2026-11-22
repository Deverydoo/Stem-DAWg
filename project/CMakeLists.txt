# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

daw_add_module(project
    SOURCES
        src/ProjectModel.cpp
        src/Metadata.cpp
        src/Migrations.cpp
        src/ProjectSerializer.cpp
        src/ProjectStore.cpp
    PUBLIC_DEPS
        daw::core)
