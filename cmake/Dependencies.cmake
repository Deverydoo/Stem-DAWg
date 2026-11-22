# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

# Third-party dependencies fetched at configure time.
# Pinned versions keep builds reproducible; bump deliberately and record the
# change in docs/milestone-1/M1-Technical-Specification.md (section 12).

include(FetchContent)

# --- JUCE: audio devices, audio file decoding (engine + media only) ---------
FetchContent_Declare(JUCE
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.7.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
FetchContent_MakeAvailable(JUCE)

# --- nlohmann/json: project manifest, analysis results, structured logs -----
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
FetchContent_MakeAvailable(nlohmann_json)

# --- Catch2: test framework ---------------------------------------------------
if(DAW_BUILD_TESTS)
    FetchContent_Declare(Catch2
        URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SYSTEM)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()
