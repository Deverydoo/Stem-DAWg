# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

# Compile options shared by every first-party DAW target.
# Third-party code (JUCE, ONNX Runtime, Qt) is consumed through SYSTEM include
# directories so its headers do not trigger our warning level.

add_library(daw_compile_options INTERFACE)
add_library(daw::compile_options ALIAS daw_compile_options)

target_compile_features(daw_compile_options INTERFACE cxx_std_20)

if(MSVC)
    target_compile_options(daw_compile_options INTERFACE
        /W4             # high warning level
        /permissive-    # standards conformance
        /utf-8          # source and execution charset are UTF-8
        /Zc:__cplusplus # report the real __cplusplus value
        /Zc:inline
        /EHsc
        /MP             # parallel compilation within a target
        /bigobj
        /wd4324         # structure padded due to alignas (intentional in lock-free queues)
    )
    if(DAW_WARNINGS_AS_ERRORS)
        target_compile_options(daw_compile_options INTERFACE /WX)
    endif()
    target_compile_definitions(daw_compile_options INTERFACE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00   # Windows 10+ APIs
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(daw_compile_options INTERFACE -Wall -Wextra -Wpedantic)
    if(DAW_WARNINGS_AS_ERRORS)
        target_compile_options(daw_compile_options INTERFACE -Werror)
    endif()
endif()
