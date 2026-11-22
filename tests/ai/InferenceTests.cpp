// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/ai/AnalysisService.h>
#include <daw/ai/Analyzers.h>
#include <daw/ai/BuiltinModels.h>
#include <daw/ai/ModelRegistry.h>
#include <daw/core/Files.h>

#include "TempDir.h"
#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <thread>

using namespace daw;
using namespace daw::test;
using namespace std::chrono_literals;

TEST_CASE("Model registry validates entries and manifests", "[ai][models]")
{
    TempDir dir("models");
    const auto modelFile = dir / "scale.onnx";
    const auto bytes = buildScaleModel(3.0f);
    REQUIRE(writeFileAtomically(modelFile, std::string(bytes.begin(), bytes.end())).ok());

    const auto manifest = dir / "models.json";
    REQUIRE(writeFileAtomically(manifest, R"({"models":[{"id":"test.scale3","version":"1","task":"test",
        "file":"scale.onnx","providers":["directml","cpu"]}]})").ok());

    ModelRegistry registry;
    const auto added = registry.loadManifest(manifest);
    REQUIRE(added.ok());
    REQUIRE(*added == 1);
    const auto model = registry.find("test.scale3");
    REQUIRE(model.has_value());
    REQUIRE(model->file == modelFile);
    REQUIRE(model->preferredProviders == std::vector<ExecutionProvider>{ExecutionProvider::DirectML, ExecutionProvider::Cpu});

    REQUIRE(registry.add(*model).error().code == ErrorCode::AlreadyExists);
    REQUIRE(registry.add(ModelDescriptor{"x", "", "", dir / "missing.onnx", {}, {}}).error().code == ErrorCode::NotFound);

    REQUIRE(writeFileAtomically(manifest, R"({"models":[{"id":"a","file":"missing.onnx"}]})").ok());
    REQUIRE(ModelRegistry{}.loadManifest(manifest).error().code == ErrorCode::NotFound);
    REQUIRE(writeFileAtomically(manifest, R"({"models":[{"id":"a","file":"scale.onnx","providers":["tpu"]}]})").ok());
    REQUIRE(ModelRegistry{}.loadManifest(manifest).error().code == ErrorCode::ParseError);
}

#if DAW_HAS_ONNX

TEST_CASE("ONNX backend initialises with at least the CPU provider", "[ai][onnx]")
{
    const auto backend = createOnnxBackend();
    REQUIRE(backend.ok());
    REQUIRE((*backend)->name().rfind("onnxruntime ", 0) == 0);
    const auto providers = (*backend)->availableProviders();
    REQUIRE(std::find(providers.begin(), providers.end(), ExecutionProvider::Cpu) != providers.end());
}

TEST_CASE("Built-in self-test model runs through ONNX Runtime", "[ai][onnx]")
{
    auto backend = *createOnnxBackend();
    const auto report = runInferenceSelfTest(*backend);
    REQUIRE(report.ok());
    REQUIRE(report->maxAbsError == 0.0);
    REQUIRE(report->provider == ExecutionProvider::Cpu);
    const auto info = backend->info(kSelfTestModelId);
    REQUIRE(info->inputNames == std::vector<std::string>{"x"});
    REQUIRE(info->outputNames == std::vector<std::string>{"y"});
}

TEST_CASE("ONNX models load from disk and run concurrently", "[ai][onnx][threads]")
{
    TempDir dir("onnx-file");
    const auto modelFile = dir / utf8ToPath("modèle ✓.onnx");
    const auto bytes = buildScaleModel(3.0f);
    REQUIRE(writeFileAtomically(modelFile, std::string(bytes.begin(), bytes.end())).ok());

    auto backend = *createOnnxBackend();
    const auto loaded = backend->load({"test.scale3", "1", "test", modelFile, {}, {ExecutionProvider::DirectML}});
    REQUIRE(loaded.ok()); // falls back to CPU when DirectML is not in this build

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 50; ++i) {
                TensorF x{{3}, {1.0f * t, 2.0f, -4.0f}};
                auto y = backend->run("test.scale3", {{"x", x}});
                if (!y || y->front().data != std::vector<float>{3.0f * t, 6.0f, -12.0f})
                    ++failures;
            }
        });
    }
    for (auto& thread : threads)
        thread.join();
    REQUIRE(failures == 0);

    REQUIRE(backend->run("not.loaded", {}).error().code == ErrorCode::NotFound);
    backend->unload("test.scale3");
    REQUIRE_FALSE(backend->isLoaded("test.scale3"));
    REQUIRE(backend->load({"bad", "", "", dir / "missing.onnx", {}, {}}).error().code == ErrorCode::NotFound);
}

TEST_CASE("Inference probe analyzer runs audio through the AI path inside a job", "[ai][onnx][service]")
{
    AnalysisService service(AnalysisServiceConfig{1}, *createOnnxBackend());
    REQUIRE(registerBuiltinAnalyzers(service).ok());
    const JobId id = *service.submit({kInferenceProbeAnalyzerId, makeSine(440.0, 0.5, 48000.0)});
    REQUIRE(service.wait(id, 30s));
    REQUIRE(service.status(id)->state == JobState::Succeeded);
}

#endif

TEST_CASE("Inference probe reports Unavailable without a backend", "[ai][service]")
{
    AnalysisService service(AnalysisServiceConfig{1});
    REQUIRE(registerBuiltinAnalyzers(service).ok());
    const JobId id = *service.submit({kInferenceProbeAnalyzerId, makeSilence(100, 48000.0, 1)});
    REQUIRE(service.wait(id, 10s));
    const auto status = service.status(id);
    REQUIRE(status->state == JobState::Failed);
    REQUIRE(status->error->code == ErrorCode::Unavailable);
}
