// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/ai/BuiltinModels.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <string_view>

namespace daw {

namespace {

// Minimal protobuf encoder covering the ONNX ModelProto fields we need.
class ProtoWriter {
public:
    void varint(std::uint64_t value)
    {
        while (value >= 0x80) {
            bytes.push_back(static_cast<std::uint8_t>(value | 0x80));
            value >>= 7;
        }
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    void tag(int field, int wireType) { varint((static_cast<std::uint64_t>(field) << 3) | static_cast<std::uint64_t>(wireType)); }
    void integer(int field, std::int64_t value)
    {
        tag(field, 0);
        varint(static_cast<std::uint64_t>(value));
    }
    void string(int field, std::string_view text)
    {
        tag(field, 2);
        varint(text.size());
        bytes.insert(bytes.end(), text.begin(), text.end());
    }
    void message(int field, const ProtoWriter& nested)
    {
        tag(field, 2);
        varint(nested.bytes.size());
        bytes.insert(bytes.end(), nested.bytes.begin(), nested.bytes.end());
    }
    void packedFloats(int field, const std::vector<float>& values)
    {
        tag(field, 2);
        varint(values.size() * sizeof(float));
        for (float v : values) {
            std::uint8_t raw[sizeof(float)];
            std::memcpy(raw, &v, sizeof(float));
            bytes.insert(bytes.end(), raw, raw + sizeof(float));
        }
    }

    std::vector<std::uint8_t> bytes;
};

constexpr int kFloatType = 1; // TensorProto.DataType.FLOAT

ProtoWriter vectorValueInfo(std::string_view name)
{
    ProtoWriter dim;
    dim.string(2, "N"); // Dimension.dim_param
    ProtoWriter shape;
    shape.message(1, dim); // TensorShapeProto.dim
    ProtoWriter tensorType;
    tensorType.integer(1, kFloatType); // elem_type
    tensorType.message(2, shape);      // shape
    ProtoWriter type;
    type.message(1, tensorType); // TypeProto.tensor_type
    ProtoWriter info;
    info.string(1, name); // ValueInfoProto.name
    info.message(2, type);
    return info;
}

} // namespace

std::vector<std::uint8_t> buildScaleModel(float factor)
{
    ProtoWriter node; // NodeProto
    node.string(1, "x");
    node.string(1, "scale");
    node.string(2, "y");
    node.string(3, "mul0");
    node.string(4, "Mul");

    ProtoWriter initializer; // TensorProto (scalar)
    initializer.integer(2, kFloatType);
    initializer.packedFloats(4, {factor});
    initializer.string(8, "scale");

    ProtoWriter graph; // GraphProto
    graph.message(1, node);
    graph.string(2, "daw_selftest");
    graph.message(5, initializer);
    graph.message(11, vectorValueInfo("x"));
    graph.message(12, vectorValueInfo("y"));

    ProtoWriter opset; // OperatorSetIdProto
    opset.string(1, "");
    opset.integer(2, 13);

    ProtoWriter model; // ModelProto
    model.integer(1, 8); // ir_version
    model.string(2, "stem-dawg");
    model.message(7, graph);
    model.message(8, opset);
    return model.bytes;
}

ModelDescriptor selfTestModelDescriptor()
{
    ModelDescriptor descriptor;
    descriptor.id = kSelfTestModelId;
    descriptor.version = "1";
    descriptor.task = "self-test";
    descriptor.inlineBytes = buildScaleModel(2.0f);
    descriptor.preferredProviders = {ExecutionProvider::Cpu};
    return descriptor;
}

Result<SelfTestReport> runInferenceSelfTest(InferenceBackend& backend)
{
    if (!backend.isLoaded(kSelfTestModelId)) {
        auto loaded = backend.load(selfTestModelDescriptor());
        if (!loaded)
            return loaded.error();
    }
    auto info = backend.info(kSelfTestModelId);
    if (!info)
        return info.error();

    TensorF input;
    input.shape = {64};
    input.data.resize(64);
    for (std::size_t i = 0; i < input.data.size(); ++i)
        input.data[i] = static_cast<float>(i) * 0.25f - 4.0f;

    const auto start = std::chrono::steady_clock::now();
    auto outputs = backend.run(kSelfTestModelId, {{"x", input}});
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!outputs)
        return outputs.error();
    if (outputs->size() != 1 || outputs->front().data.size() != input.data.size())
        return makeError(ErrorCode::InferenceError, "self-test model returned an unexpected shape");

    SelfTestReport report;
    report.backendName = backend.name();
    report.provider = info->provider;
    report.latencyMs = elapsed;
    for (std::size_t i = 0; i < input.data.size(); ++i)
        report.maxAbsError = std::max(report.maxAbsError,
                                      static_cast<double>(std::abs(outputs->front().data[i] - 2.0f * input.data[i])));
    if (report.maxAbsError > 1e-6)
        return makeError(ErrorCode::InferenceError, "self-test output mismatch");
    return report;
}

} // namespace daw
