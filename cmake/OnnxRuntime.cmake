# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

# Imports ONNX Runtime as onnxruntime::onnxruntime.
#
# Default: downloads the official Microsoft.ML.OnnxRuntime NuGet package
# (CPU, win-x64) at DAW_ONNXRUNTIME_VERSION. NuGet packages are zip files with
# headers in build/native/include and binaries in runtimes/win-x64/native.
#
# Alternatively set DAW_ONNXRUNTIME_ROOT to an extracted package in either the
# NuGet layout or the release-zip layout (include/ + lib/). The DirectML
# (Microsoft.ML.OnnxRuntime.DirectML) and GPU packages use the same layouts.
#
# IMPORTANT (Windows): System32 ships an older onnxruntime.dll for Windows ML.
# The package DLLs are copied next to every executable (daw_copy_runtime_dlls)
# so the loader never picks up the system copy.

if(DAW_ONNXRUNTIME_ROOT)
    set(_daw_ort_root "${DAW_ONNXRUNTIME_ROOT}")
else()
    if(NOT WIN32)
        message(FATAL_ERROR "Automatic ONNX Runtime download is only wired for Windows x64. Set DAW_ONNXRUNTIME_ROOT.")
    endif()
    FetchContent_Declare(onnxruntime_bin
        URL "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime/${DAW_ONNXRUNTIME_VERSION}"
        DOWNLOAD_NAME "microsoft.ml.onnxruntime.${DAW_ONNXRUNTIME_VERSION}.zip"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(onnxruntime_bin)
    set(_daw_ort_root "${onnxruntime_bin_SOURCE_DIR}")
endif()

if(EXISTS "${_daw_ort_root}/build/native/include/onnxruntime_cxx_api.h")
    set(_daw_ort_include "${_daw_ort_root}/build/native/include")
    set(_daw_ort_bin "${_daw_ort_root}/runtimes/win-x64/native")
elseif(EXISTS "${_daw_ort_root}/include/onnxruntime_cxx_api.h")
    set(_daw_ort_include "${_daw_ort_root}/include")
    set(_daw_ort_bin "${_daw_ort_root}/lib")
else()
    message(FATAL_ERROR "ONNX Runtime headers not found under ${_daw_ort_root}")
endif()

find_library(DAW_ORT_IMPLIB NAMES onnxruntime PATHS "${_daw_ort_bin}" NO_DEFAULT_PATH NO_CACHE REQUIRED)
find_file(DAW_ORT_DLL NAMES onnxruntime.dll PATHS "${_daw_ort_bin}" NO_DEFAULT_PATH NO_CACHE REQUIRED)
file(GLOB DAW_ONNXRUNTIME_DLLS "${_daw_ort_bin}/*.dll")

add_library(onnxruntime::onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime::onnxruntime PROPERTIES
    IMPORTED_LOCATION             "${DAW_ORT_DLL}"
    IMPORTED_IMPLIB               "${DAW_ORT_IMPLIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${_daw_ort_include}")

message(STATUS "ONNX Runtime ${DAW_ONNXRUNTIME_VERSION}: ${_daw_ort_root}")
