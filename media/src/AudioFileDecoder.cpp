// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/media/AudioFileDecoder.h>

#include <daw/core/Files.h>

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <memory>

namespace fs = std::filesystem;

namespace daw {

namespace {

juce::File toJuceFile(const fs::path& path)
{
    return juce::File(juce::String::fromUTF8(pathToUtf8(fs::absolute(path)).c_str()));
}

Result<std::unique_ptr<juce::AudioFormatReader>> openReader(const fs::path& path)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
        return makeError(ErrorCode::NotFound, "audio file not found: " + pathToUtf8(path));

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(toJuceFile(path)));
    if (!reader)
        return makeError(ErrorCode::UnsupportedFormat, "unsupported or unreadable audio file: " + pathToUtf8(path));
    if (reader->sampleRate <= 0.0 || reader->numChannels == 0)
        return makeError(ErrorCode::Corrupt, "audio file has no channels or an invalid sample rate: " + pathToUtf8(path));
    return reader;
}

} // namespace

Result<AudioFileInfo> probeAudioFile(const fs::path& path)
{
    auto reader = openReader(path);
    if (!reader)
        return reader.error();
    const auto& r = **reader;
    AudioFileInfo info;
    info.sampleRate = r.sampleRate;
    info.numChannels = static_cast<int>(r.numChannels);
    info.numFrames = r.lengthInSamples;
    info.bitsPerSample = static_cast<int>(r.bitsPerSample);
    info.formatName = r.getFormatName().toStdString();
    return info;
}

Result<AudioAssetPtr> decodeAudioFile(const fs::path& path, const DecodeOptions& options,
                                      const CancellationToken& cancel, const ProgressCallback& progress)
{
    auto opened = openReader(path);
    if (!opened)
        return opened.error();
    auto& reader = **opened;

    const int channels = static_cast<int>(reader.numChannels);
    const std::int64_t frames = reader.lengthInSamples;

    if (channels > options.maxChannels)
        return makeError(ErrorCode::LimitExceeded, "file has " + std::to_string(channels) + " channels; limit is "
                                                       + std::to_string(options.maxChannels));
    if (frames > options.maxFrames)
        return makeError(ErrorCode::LimitExceeded, "file has " + std::to_string(frames) + " frames; limit is "
                                                       + std::to_string(options.maxFrames));
    const std::size_t bytes = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels) * sizeof(float);
    if (bytes > options.maxBytes)
        return makeError(ErrorCode::LimitExceeded, "decoded size exceeds the in-memory budget");

    auto asset = AudioAsset::create(channels, frames, reader.sampleRate);
    asset->sourcePath = pathToUtf8(path);

    constexpr std::int64_t chunk = 1 << 16;
    std::vector<float*> destinations(static_cast<std::size_t>(channels));
    for (std::int64_t position = 0; position < frames; position += chunk) {
        if (cancel.isCancelled())
            return makeError(ErrorCode::Cancelled, "decode cancelled");

        const int count = static_cast<int>(std::min(chunk, frames - position));
        for (int c = 0; c < channels; ++c)
            destinations[static_cast<std::size_t>(c)] = asset->channels[static_cast<std::size_t>(c)].data() + position;

        if (!reader.read(destinations.data(), channels, position, count))
            return makeError(ErrorCode::IoError, "read failed at frame " + std::to_string(position));

        if (progress)
            progress(static_cast<float>(position + count) / static_cast<float>(std::max<std::int64_t>(frames, 1)));
    }
    if (frames == 0 && progress)
        progress(1.0f);

    return AudioAssetPtr(std::move(asset));
}

std::vector<std::string> supportedAudioExtensions()
{
    return {".wav", ".aif", ".aiff", ".flac", ".ogg", ".mp3"};
}

} // namespace daw
