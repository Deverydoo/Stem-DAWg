# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

cmake_minimum_required(VERSION 3.25)

project(StemDAWg
    VERSION 0.1.0
    DESCRIPTION "Stem DAWg - music cleaner and mastering tool for AI-generated stems and home recordings"
    LANGUAGES C CXX)

if(NOT WIN32)
    message(WARNING "Milestone 1 targets Windows 11 x64 only. Other platforms are untested.")
endif()

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# All executables and runtime DLLs land in one directory per configuration,
# so ONNX Runtime / Qt DLLs are deployed once and shared by app and tests.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(DAW_BUILD_APP            "Build the Qt desktop application"                    ON)
option(DAW_BUILD_TESTS          "Build unit, integration and regression tests"        ON)
option(DAW_ENABLE_ONNX          "Build the ONNX Runtime inference backend"            ON)
option(DAW_ENABLE_ASIO          "Enable the ASIO device type (needs the ASIO SDK)"    OFF)
option(DAW_WARNINGS_AS_ERRORS   "Treat compiler warnings as errors in DAW targets"    OFF)

set(DAW_ASIO_SDK_DIR        ""       CACHE PATH   "Root of the Steinberg ASIO SDK (contains common/iasiodrv.h)")
set(DAW_ONNXRUNTIME_ROOT    ""       CACHE PATH   "Pre-extracted ONNX Runtime package (NuGet or release-zip layout). Empty = download from NuGet")
set(DAW_ONNXRUNTIME_VERSION "1.22.0" CACHE STRING "Microsoft.ML.OnnxRuntime NuGet version downloaded when DAW_ONNXRUNTIME_ROOT is empty")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerSettings)
include(Dependencies)
if(DAW_ENABLE_ONNX)
    include(OnnxRuntime)
endif()
include(DawHelpers)

# ---------------------------------------------------------------------------
# Modules (dependency order). Each module is a static library with a public
# include/ tree and a private src/ tree. Allowed dependencies are documented in
# docs/milestone-1/M1-Technical-Specification.md section 3.2 and enforced here:
#
#   core      <- std, nlohmann_json              (no JUCE, no Qt)
#   media     <- core, JUCE (decode only)        (no Qt)
#   engine    <- core, JUCE (devices only)       (no Qt)
#   project   <- core                            (no JUCE, no Qt)
#   session   <- core, media, engine, project    (no Qt)
#   ai        <- core, ONNX Runtime              (no JUCE, no Qt)
#   apps/*    <- everything + Qt
# ---------------------------------------------------------------------------
add_subdirectory(third_party/juce_static)
add_subdirectory(core)
add_subdirectory(media)
add_subdirectory(engine)
add_subdirectory(project)
add_subdirectory(session)
add_subdirectory(ai)

if(DAW_BUILD_APP)
    add_subdirectory(apps/desktop)
endif()

if(DAW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "Stem DAWg ${PROJECT_VERSION}: app=${DAW_BUILD_APP} tests=${DAW_BUILD_TESTS} onnx=${DAW_ENABLE_ONNX} asio=${DAW_ENABLE_ASIO}")
