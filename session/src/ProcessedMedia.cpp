// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "ProcessedMedia.h"

#include <daw/core/Files.h>
#include <daw/media/PeakCache.h>
#include <daw/media/WavWriter.h>
#include <daw/session/MediaLoader.h>

#include <algorithm>
#include <cmath>

namespace fs = std::filesystem;

namespace daw::detail {

std::vector<double> blockPowers(const AudioAsset& audio)
{
    const auto block = static_cast<std::int64_t>(std::max(1.0, std::round(audio.sampleRate * 0.02)));
    std::vector<double> powers;
    for (std::int64_t start = 0; start + block <= audio.numFrames(); start += block) {
        double sum = 0.0;
        for (const auto& channel : audio.channels)
            for (std::int64_t i = start; i < start + block; ++i)
                sum += static_cast<double>(channel[static_cast<std::size_t>(i)]) * channel[static_cast<std::size_t>(i)];
        powers.push_back(sum / static_cast<double>(block * std::max(1, audio.numChannels())));
    }
    return powers;
}

double levelChange(const std::vector<double>& before, const std::vector<double>& after, double low, double high)
{
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < before.size() && i < after.size(); ++i)
        if (before[i] > 1e-12) // not digital silence
            order.push_back(i);
    if (order.empty())
        return 0.0;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return before[a] < before[b]; });
    const auto from = static_cast<std::size_t>(low * static_cast<double>(order.size()));
    const auto to = std::max(from + 1, static_cast<std::size_t>(high * static_cast<double>(order.size())));
    double b = 0.0, a = 0.0;
    for (std::size_t k = from; k < to && k < order.size(); ++k) {
        b += before[order[k]];
        a += after[order[k]];
    }
    return b > 0.0 ? 10.0 * std::log10(std::max(a, 1e-30) / b) : 0.0;
}

Result<ProcessedCopy> writeProcessedCopy(const fs::path& projectRoot, const MediaRef& original, const AudioAsset& audio,
                                         const std::string& suffix, const std::string& process, double amountDb,
                                         double engineRate, const CancellationToken& cancel)
{
    std::error_code ec;
    const fs::path dir = projectRoot / utf8ToPath(kProcessedDir);
    fs::create_directories(dir, ec);
    if (ec)
        return makeError(ErrorCode::IoError, "cannot create " + pathToUtf8(dir) + ": " + ec.message());
    const std::string stem = (original.name.empty() ? std::string("Track") : original.name) + " (" + suffix + ")";
    std::string fileName = stem + ".wav";
    for (int n = 2; fs::exists(dir / utf8ToPath(fileName), ec); ++n)
        fileName = stem + " " + std::to_string(n) + ".wav";
    if (auto status = writeWavFile(dir / utf8ToPath(fileName), audio, WavSampleFormat::Float32); !status)
        return status.error();

    ProcessedCopy copy;
    MediaRef& ref = copy.media;
    ref.id = MediaId::generate();
    ref.name = stem;
    ref.path = std::string(kProcessedDir) + "/" + fileName;
    ref.storage = MediaStorage::Copied;
    ref.sampleRate = audio.sampleRate;
    ref.numChannels = audio.numChannels();
    ref.numFrames = audio.numFrames();
    ref.derivation = MediaDerivation{original.id, process, amountDb};
    auto key = computeSourceKey(dir / utf8ToPath(fileName));
    if (!key)
        return key.error();
    ref.sourceKey = *key;

    MediaLoadOptions load;
    load.engineSampleRate = engineRate;
    auto loaded = loadMedia(projectRoot, ref, load, cancel);
    if (!loaded)
        return loaded.error();
    copy.loaded = std::move(*loaded);
    return copy;
}

} // namespace daw::detail
