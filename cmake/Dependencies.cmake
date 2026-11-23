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

# --- TinySoundFont + TinyMidiLoader: MIDI file playback through SoundFonts ---
# Single-header C libraries (MIT). No upstream release tags: pinned by commit.
FetchContent_Declare(tinysoundfont
    URL https://github.com/schellingb/TinySoundFont/archive/a3621a19c6504885cd9583e6e26c35bc954fc8ec.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(tinysoundfont)
add_library(daw_tinysoundfont INTERFACE)
add_library(daw::tinysoundfont ALIAS daw_tinysoundfont)
target_include_directories(daw_tinysoundfont SYSTEM INTERFACE "${tinysoundfont_SOURCE_DIR}")

# --- XZ Embedded: .xz decompression for the SoundFont package (0BSD) ----------
FetchContent_Declare(xz_embedded
    URL https://github.com/tukaani-project/xz-embedded/archive/refs/tags/v2024-12-30.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(xz_embedded)
add_library(daw_xz STATIC
    "${xz_embedded_SOURCE_DIR}/linux/lib/xz/xz_crc32.c"
    "${xz_embedded_SOURCE_DIR}/linux/lib/xz/xz_crc64.c"
    "${xz_embedded_SOURCE_DIR}/linux/lib/xz/xz_dec_stream.c"
    "${xz_embedded_SOURCE_DIR}/linux/lib/xz/xz_dec_lzma2.c"
    "${xz_embedded_SOURCE_DIR}/linux/lib/xz/xz_dec_bcj.c")
add_library(daw::xz ALIAS daw_xz)
target_include_directories(daw_xz SYSTEM PUBLIC
    "${xz_embedded_SOURCE_DIR}/linux/include/linux"
    "${xz_embedded_SOURCE_DIR}/userspace")
target_compile_definitions(daw_xz PUBLIC XZ_USE_CRC64 XZ_DEC_ANY_CHECK)
set_target_properties(daw_xz PROPERTIES FOLDER "third_party")
if(MSVC)
    target_compile_options(daw_xz PRIVATE /W0)
endif()
