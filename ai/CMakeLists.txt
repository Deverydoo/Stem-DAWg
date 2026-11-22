# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

daw_add_module(ai
    SOURCES
        src/AnalysisService.cpp
        src/ModelRegistry.cpp
        src/BuiltinModels.cpp
        src/OnnxBackend.cpp
        src/Fft.cpp
        src/LevelAnalyzer.cpp
        src/TransientAnalyzer.cpp
        src/SpectralAnalyzer.cpp
        src/InferenceProbeAnalyzer.cpp
        src/Analyzers.cpp
    PUBLIC_DEPS
        daw::core)

if(DAW_ENABLE_ONNX)
    target_link_libraries(daw_ai PRIVATE onnxruntime::onnxruntime)
    target_compile_definitions(daw_ai PUBLIC DAW_HAS_ONNX=1)
else()
    target_compile_definitions(daw_ai PUBLIC DAW_HAS_ONNX=0)
endif()
