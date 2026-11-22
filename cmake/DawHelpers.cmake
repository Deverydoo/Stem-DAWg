# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

# Helper functions shared by module and test CMakeLists.

# daw_add_module(<name> SOURCES ... [PUBLIC_DEPS ...] [PRIVATE_DEPS ...])
# Creates static library daw_<name> with alias daw::<name>, a public include/
# directory and the shared compile options.
function(daw_add_module name)
    cmake_parse_arguments(M "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})
    set(target daw_${name})
    add_library(${target} STATIC ${M_SOURCES})
    add_library(daw::${name} ALIAS ${target})
    target_include_directories(${target}
        PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${target}
        PUBLIC  ${M_PUBLIC_DEPS}
        PRIVATE daw::compile_options ${M_PRIVATE_DEPS})
    set_target_properties(${target} PROPERTIES FOLDER "modules")
endfunction()

# daw_copy_runtime_dlls(<target>)
# Copies third-party runtime DLLs (ONNX Runtime) next to an executable.
function(daw_copy_runtime_dlls target)
    if(DAW_ENABLE_ONNX AND DAW_ONNXRUNTIME_DLLS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DAW_ONNXRUNTIME_DLLS} "$<TARGET_FILE_DIR:${target}>"
            COMMENT "Deploying ONNX Runtime DLLs for ${target}"
            VERBATIM)
    endif()
endfunction()
