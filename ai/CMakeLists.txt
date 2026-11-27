# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

# The Basic Pitch model (230 KB) is compiled in, so nothing has to be deployed
# next to the app, the tools or the tests.
set(DAW_BASIC_PITCH_MODEL "${PROJECT_SOURCE_DIR}/third_party/basic-pitch/nmp.onnx")
set(DAW_BASIC_PITCH_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/BasicPitchModel.cpp")
add_custom_command(
    OUTPUT "${DAW_BASIC_PITCH_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" -DINPUT=${DAW_BASIC_PITCH_MODEL} -DOUTPUT=${DAW_BASIC_PITCH_SOURCE}
            -DSYMBOL=kBasicPitchModel -P "${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake"
    DEPENDS "${DAW_BASIC_PITCH_MODEL}" "${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake"
    COMMENT "Embedding the Basic Pitch model"
    VERBATIM)

daw_add_module(ai
    SOURCES
        src/AnalysisService.cpp
        src/ModelRegistry.cpp
        src/BuiltinModels.cpp
        src/OnnxBackend.cpp
        src/LevelAnalyzer.cpp
        src/TransientAnalyzer.cpp
        src/SpectralAnalyzer.cpp
        src/InferenceProbeAnalyzer.cpp
        src/VoiceDenoiser.cpp
        src/NoteTranscriber.cpp
        "${DAW_BASIC_PITCH_SOURCE}"
        src/Analyzers.cpp
    PUBLIC_DEPS
        daw::core
        daw::dsp)

if(DAW_ENABLE_ONNX)
    target_link_libraries(daw_ai PRIVATE onnxruntime::onnxruntime)
    target_compile_definitions(daw_ai PUBLIC DAW_HAS_ONNX=1)
else()
    target_compile_definitions(daw_ai PUBLIC DAW_HAS_ONNX=0)
endif()
