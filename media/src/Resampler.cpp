// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/media/Resampler.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

namespace daw {

namespace {

double besselI0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    const double halfX = x * 0.5;
    for (int k = 1; k < 64; ++k) {
        term *= (halfX / k) * (halfX / k);
        sum += term;
        if (term < sum * 1e-12)
            break;
    }
    return sum;
}

class KernelTable {
public:
    static constexpr int kPhases = 512; // table entries per input sample

    KernelTable(double cutoff, double support, double beta) : support_(support)
    {
        const int size = static_cast<int>(std::ceil(support * kPhases)) + 2;
        table_.resize(static_cast<std::size_t>(size));
        const double i0Beta = besselI0(beta);
        for (int j = 0; j < size; ++j) {
            const double d = static_cast<double>(j) / kPhases;
            if (d >= support) {
                table_[static_cast<std::size_t>(j)] = 0.0f;
                continue;
            }
            const double x = cutoff * d;
            const double sinc = x == 0.0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
            const double r = d / support;
            const double window = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0Beta;
            table_[static_cast<std::size_t>(j)] = static_cast<float>(cutoff * sinc * window);
        }
    }

    float at(double distance) const noexcept
    {
        const double pos = std::abs(distance) * kPhases;
        const auto index = static_cast<std::size_t>(pos);
        if (index + 1 >= table_.size())
            return 0.0f;
        const auto frac = static_cast<float>(pos - static_cast<double>(index));
        return table_[index] + frac * (table_[index + 1] - table_[index]);
    }

    double support() const noexcept { return support_; }

private:
    double support_;
    std::vector<float> table_;
};

} // namespace

Result<AudioAssetPtr> resampleAudio(const AudioAsset& input, double targetRate, const ResampleOptions& options,
                                    const CancellationToken& cancel)
{
    if (input.sampleRate <= 0.0 || targetRate <= 0.0)
        return makeError(ErrorCode::InvalidArgument, "invalid sample rate for resampling");

    const double ratio = targetRate / input.sampleRate;
    const std::int64_t inFrames = input.numFrames();
    const auto outFrames = static_cast<std::int64_t>(std::llround(static_cast<double>(inFrames) * ratio));
    auto output = AudioAsset::create(input.numChannels(), outFrames, targetRate);
    output->sourcePath = input.sourcePath;

    if (input.sampleRate == targetRate) {
        output->channels = input.channels;
        return AudioAssetPtr(std::move(output));
    }

    const double cutoff = std::min(1.0, ratio) * options.cutoffMargin; // relative to input Nyquist*2 (cycles/sample)
    const double support = options.halfTaps / cutoff;                  // kernel half-width in input samples
    const KernelTable kernel(cutoff, support, options.kaiserBeta);

    std::atomic<bool> cancelled{false};
    auto convertChannel = [&](int channel) {
        const float* in = input.channelData(channel);
        float* out = output->channels[static_cast<std::size_t>(channel)].data();
        for (std::int64_t n = 0; n < outFrames; ++n) {
            if ((n & 0xFFFF) == 0 && cancel.isCancelled()) {
                cancelled.store(true);
                return;
            }
            const double t = static_cast<double>(n) / ratio;
            const auto first = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(t - support)) + 1);
            const auto last = std::min<std::int64_t>(inFrames - 1, static_cast<std::int64_t>(std::floor(t + support)));
            double acc = 0.0;
            for (std::int64_t i = first; i <= last; ++i)
                acc += static_cast<double>(in[i]) * kernel.at(t - static_cast<double>(i));
            out[n] = static_cast<float>(acc);
        }
    };

    std::vector<std::thread> workers;
    for (int c = 1; c < input.numChannels(); ++c)
        workers.emplace_back(convertChannel, c);
    if (input.numChannels() > 0)
        convertChannel(0);
    for (auto& worker : workers)
        worker.join();

    if (cancelled.load())
        return makeError(ErrorCode::Cancelled, "resampling cancelled");
    return AudioAssetPtr(std::move(output));
}

} // namespace daw
