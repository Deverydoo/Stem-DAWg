// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/ai/InferenceBackend.h>

#include <daw/core/Files.h>
#include <daw/core/Logger.h>

#include <algorithm>
#include <map>
#include <mutex>

#if DAW_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace daw {

const char* toString(ExecutionProvider provider) noexcept
{
    switch (provider) {
    case ExecutionProvider::Cpu: return "cpu";
    case ExecutionProvider::DirectML: return "directml";
    case ExecutionProvider::Cuda: return "cuda";
    }
    return "cpu";
}

#if DAW_HAS_ONNX

namespace {

const char* ortProviderName(ExecutionProvider provider)
{
    switch (provider) {
    case ExecutionProvider::Cpu: return "CPUExecutionProvider";
    case ExecutionProvider::DirectML: return "DmlExecutionProvider";
    case ExecutionProvider::Cuda: return "CUDAExecutionProvider";
    }
    return "CPUExecutionProvider";
}

class OnnxBackend final : public InferenceBackend {
public:
    explicit OnnxBackend(const OnnxBackendConfig& config)
        : config_(config), env_(ORT_LOGGING_LEVEL_WARNING, "stem-dawg")
    {
        for (const std::string& name : Ort::GetAvailableProviders()) {
            for (ExecutionProvider p : {ExecutionProvider::Cpu, ExecutionProvider::DirectML, ExecutionProvider::Cuda})
                if (name == ortProviderName(p))
                    available_.push_back(p);
        }
        if (std::find(available_.begin(), available_.end(), ExecutionProvider::Cpu) == available_.end())
            available_.push_back(ExecutionProvider::Cpu);
    }

    std::string name() const override { return std::string("onnxruntime ") + OrtGetApiBase()->GetVersionString(); }

    std::vector<ExecutionProvider> availableProviders() const override { return available_; }

    Result<LoadedModelInfo> load(const ModelDescriptor& model) override
    {
        if (model.id.empty())
            return makeError(ErrorCode::InvalidArgument, "model id is empty");
        try {
            Ort::SessionOptions options;
            options.SetIntraOpNumThreads(std::max(1, config_.intraOpThreads));
            options.SetInterOpNumThreads(1);
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

            ExecutionProvider chosen = ExecutionProvider::Cpu;
            for (ExecutionProvider wanted : model.preferredProviders) {
                if (wanted == ExecutionProvider::Cpu)
                    break;
                if (std::find(available_.begin(), available_.end(), wanted) == available_.end())
                    continue;
                try {
                    if (wanted == ExecutionProvider::Cuda)
                        options.AppendExecutionProvider_CUDA(OrtCUDAProviderOptions{});
                    else
                        options.AppendExecutionProvider("DML", {});
                    chosen = wanted;
                    break;
                } catch (const Ort::Exception& e) {
                    logging::warn("ai", "execution provider unavailable; falling back",
                                  {{"model", model.id}, {"provider", toString(wanted)}, {"error", e.what()}});
                }
            }

            std::unique_ptr<Ort::Session> session;
            if (!model.inlineBytes.empty()) {
                session = std::make_unique<Ort::Session>(env_, model.inlineBytes.data(), model.inlineBytes.size(), options);
            } else {
                std::error_code ec;
                if (!std::filesystem::is_regular_file(model.file, ec))
                    return makeError(ErrorCode::NotFound, "model file not found: " + pathToUtf8(model.file));
                session = std::make_unique<Ort::Session>(env_, model.file.c_str(), options);
            }

            auto entry = std::make_shared<Entry>();
            entry->info.id = model.id;
            entry->info.provider = chosen;
            Ort::AllocatorWithDefaultOptions allocator;
            for (std::size_t i = 0; i < session->GetInputCount(); ++i)
                entry->info.inputNames.emplace_back(session->GetInputNameAllocated(i, allocator).get());
            for (std::size_t i = 0; i < session->GetOutputCount(); ++i)
                entry->info.outputNames.emplace_back(session->GetOutputNameAllocated(i, allocator).get());
            entry->session = std::move(session);

            std::lock_guard lock(mutex_);
            models_[model.id] = entry;
            logging::info("ai", "model loaded", {{"model", model.id}, {"provider", toString(chosen)}});
            return entry->info;
        } catch (const Ort::Exception& e) {
            return makeError(ErrorCode::InferenceError, "failed to load model '" + model.id + "': " + e.what());
        }
    }

    bool isLoaded(const std::string& modelId) const override
    {
        std::lock_guard lock(mutex_);
        return models_.contains(modelId);
    }

    Result<LoadedModelInfo> info(const std::string& modelId) const override
    {
        auto entry = find(modelId);
        if (!entry)
            return makeError(ErrorCode::NotFound, "model not loaded: " + modelId);
        return entry->info;
    }

    void unload(const std::string& modelId) override
    {
        std::lock_guard lock(mutex_);
        models_.erase(modelId);
    }

    Result<std::vector<TensorF>> run(const std::string& modelId,
                                     const std::vector<std::pair<std::string, TensorF>>& inputs,
                                     const CancellationToken& cancel) override
    {
        auto entry = find(modelId);
        if (!entry)
            return makeError(ErrorCode::NotFound, "model not loaded: " + modelId);
        if (cancel.isCancelled())
            return makeError(ErrorCode::Cancelled, "inference cancelled");

        try {
            const Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            std::vector<Ort::Value> values;
            std::vector<const char*> inputNames;
            values.reserve(inputs.size());
            for (const auto& [name, tensor] : inputs) {
                inputNames.push_back(name.c_str());
                values.push_back(Ort::Value::CreateTensor<float>(memory, const_cast<float*>(tensor.data.data()),
                                                                 tensor.data.size(), tensor.shape.data(), tensor.shape.size()));
            }
            std::vector<const char*> outputNames;
            for (const std::string& name : entry->info.outputNames)
                outputNames.push_back(name.c_str());

            auto outputs = entry->session->Run(Ort::RunOptions{nullptr}, inputNames.data(), values.data(), values.size(),
                                               outputNames.data(), outputNames.size());

            std::vector<TensorF> result;
            for (auto& value : outputs) {
                const auto shapeInfo = value.GetTensorTypeAndShapeInfo();
                if (shapeInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
                    return makeError(ErrorCode::InferenceError, "model output is not float32");
                TensorF tensor;
                tensor.shape = shapeInfo.GetShape();
                const float* data = value.GetTensorData<float>();
                tensor.data.assign(data, data + shapeInfo.GetElementCount());
                result.push_back(std::move(tensor));
            }
            return result;
        } catch (const Ort::Exception& e) {
            return makeError(ErrorCode::InferenceError, "inference failed for '" + modelId + "': " + e.what());
        }
    }

private:
    struct Entry {
        LoadedModelInfo info;
        std::unique_ptr<Ort::Session> session; // Session::Run is thread-safe
    };

    std::shared_ptr<Entry> find(const std::string& modelId) const
    {
        std::lock_guard lock(mutex_);
        const auto it = models_.find(modelId);
        return it == models_.end() ? nullptr : it->second;
    }

    OnnxBackendConfig config_;
    Ort::Env env_;
    std::vector<ExecutionProvider> available_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Entry>> models_;
};

} // namespace

Result<std::shared_ptr<InferenceBackend>> createOnnxBackend(const OnnxBackendConfig& config)
{
    try {
        return std::shared_ptr<InferenceBackend>(std::make_shared<OnnxBackend>(config));
    } catch (const std::exception& e) {
        return makeError(ErrorCode::Unavailable, std::string("ONNX Runtime initialisation failed: ") + e.what());
    }
}

#else

Result<std::shared_ptr<InferenceBackend>> createOnnxBackend(const OnnxBackendConfig&)
{
    return makeError(ErrorCode::Unavailable, "built without ONNX Runtime (DAW_ENABLE_ONNX=OFF)");
}

#endif

} // namespace daw
