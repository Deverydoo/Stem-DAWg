# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

include(Catch)

# Shared fixtures: synthetic signal generators and temporary directories.
# Test audio is generated at runtime; no binary fixtures are committed.
add_library(daw_test_support STATIC
    support/TestSignals.cpp
    support/TempDir.cpp)
target_include_directories(daw_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(daw_test_support PUBLIC daw::core PRIVATE daw::compile_options)
set_target_properties(daw_test_support PROPERTIES FOLDER "tests")

# daw_add_test(<name> SOURCES ... LIBS ...)
function(daw_add_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${T_SOURCES})
    target_link_libraries(${name} PRIVATE daw_test_support Catch2::Catch2WithMain daw::compile_options ${T_LIBS})
    set_target_properties(${name} PROPERTIES FOLDER "tests")
    daw_copy_runtime_dlls(${name})
    catch_discover_tests(${name}
        DISCOVERY_MODE PRE_TEST
        TEST_PREFIX "${name}::"
        PROPERTIES TIMEOUT 120)
endfunction()

daw_add_test(daw_core_tests
    SOURCES
        core/SpscQueueTests.cpp
        core/SnapshotExchangeTests.cpp
        core/CoreUtilityTests.cpp
    LIBS daw::core)

# The dsp and engine test binaries replace global operator new/delete
# (AllocationGuard) to prove real-time code is allocation-free.
daw_add_test(daw_dsp_tests
    SOURCES
        support/AllocationGuard.cpp
        dsp/FftTests.cpp
        dsp/DeepFilterTests.cpp
        dsp/LoudnessTests.cpp
        dsp/CleanupChainTests.cpp
        dsp/DenoiseTests.cpp
        dsp/BleedTests.cpp
        dsp/TransientTests.cpp
        dsp/ResonanceTests.cpp
        dsp/StereoTests.cpp
        dsp/MasterTests.cpp
        dsp/TonalTests.cpp
        dsp/DynamicsTests.cpp
    LIBS daw::dsp)

# The engine test binary replaces global operator new/delete (AllocationGuard)
# to prove the audio callback is allocation-free.
daw_add_test(daw_engine_tests
    SOURCES
        support/AllocationGuard.cpp
        engine/TransportTests.cpp
        engine/AudioEngineTests.cpp
        engine/RealtimeSafetyTests.cpp
        engine/CleanupEngineTests.cpp
        engine/DeclickTests.cpp
        engine/MasterEngineTests.cpp
    LIBS daw::engine)

daw_add_test(daw_media_tests
    SOURCES
        media/DecoderTests.cpp
        media/PeakPyramidTests.cpp
        media/PeakCacheTests.cpp
        media/ResamplerTests.cpp
        media/AudioTagsTests.cpp
        media/MidiTests.cpp
        media/Rf64Tests.cpp
        media/ArchiveTests.cpp
    LIBS daw::media)

daw_add_test(daw_project_tests
    SOURCES
        project/SerializationTests.cpp
        project/MigrationTests.cpp
        project/ProjectStoreTests.cpp
        project/MetadataTests.cpp
    LIBS daw::project)

daw_add_test(daw_ai_tests
    SOURCES
        ai/AnalyzerTests.cpp
        ai/AnalysisServiceTests.cpp
        ai/InferenceTests.cpp
    LIBS daw::ai)

# End-to-end, device-free: the Milestone 1 definition of done.
daw_add_test(daw_integration_tests
    SOURCES
        integration/SessionTests.cpp
        integration/DefinitionOfDoneTests.cpp
        integration/RenderTests.cpp
        integration/StemReportTests.cpp
        integration/UndoTests.cpp
        integration/NoiseToolsTests.cpp
        integration/VoiceToolsTests.cpp
        integration/BleedToolsTests.cpp
        integration/MasteringTests.cpp
    LIBS daw::session daw::ai)
