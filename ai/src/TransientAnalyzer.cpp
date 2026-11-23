// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/ai/Analyzers.h>
#include <daw/dsp/Fft.h>

#include "DspUtil.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace daw {

namespace {

// Spectral-flux onset detector with an adaptive median threshold and
// sample-accurate refinement against the amplitude envelope. Deterministic
// baseline for the Milestone 6 learned kick/snare onset classifier.
class TransientAnalyzer final : public IAnalyzer {
public:
    std::string id() const override { return kTransientAnalyzerId; }
    int version() const override { return 1; }
    std::string displayName() const override { return "Transient candidates"; }

    Result<AnalysisResult> analyze(const AudioAsset& audio, const nlohmann::json& params, AnalysisContext& context) override
    {
        const int frameSize = params.value("frameSize", 1024);
        const int hop = params.value("hop", 256);
        const double sensitivity = params.value("sensitivity", 1.5);
        const double minIntervalMs = params.value("minIntervalMs", 30.0);
        const int medianRadius = params.value("medianRadius", 8);
        if (frameSize < 64 || (frameSize & (frameSize - 1)) != 0 || hop <= 0 || hop > frameSize)
            return makeError(ErrorCode::InvalidArgument, "frameSize must be a power of two >= 64 and 0 < hop <= frameSize");

        const std::vector<float> mono = detail::monoMix(audio);
        const auto length = static_cast<std::int64_t>(mono.size());
        const auto numFrames = static_cast<int>(length / hop + 1);

        const Fft fft(frameSize);
        const std::vector<float> window = hannWindow(frameSize);
        std::vector<float> frame(static_cast<std::size_t>(frameSize));
        std::vector<float> power, previous(static_cast<std::size_t>(frameSize / 2 + 1), 0.0f), current;
        std::vector<std::complex<float>> scratch;
        std::vector<double> flux(static_cast<std::size_t>(numFrames), 0.0);
        std::vector<double> frameRms(static_cast<std::size_t>(numFrames), 0.0);
        constexpr double gamma = 100.0; // log compression

        for (int f = 0; f < numFrames; ++f) {
            if ((f & 0xFF) == 0) {
                if (context.cancel.isCancelled())
                    return makeError(ErrorCode::Cancelled, "transient analysis cancelled");
                context.progress(0.8f * static_cast<float>(f) / static_cast<float>(numFrames));
            }
            const std::int64_t start = static_cast<std::int64_t>(f) * hop;
            double energy = 0.0;
            for (int i = 0; i < frameSize; ++i) {
                const std::int64_t index = start + i;
                const float sample = index < length ? mono[static_cast<std::size_t>(index)] : 0.0f;
                energy += static_cast<double>(sample) * sample;
                frame[static_cast<std::size_t>(i)] = sample * window[static_cast<std::size_t>(i)];
            }
            frameRms[static_cast<std::size_t>(f)] = std::sqrt(energy / frameSize);
            fft.powerSpectrum(frame.data(), power, scratch);
            current.resize(power.size());
            double sum = 0.0;
            for (std::size_t k = 0; k < power.size(); ++k) {
                current[k] = static_cast<float>(std::log1p(gamma * std::sqrt(static_cast<double>(power[k]))));
                sum += std::max(0.0, static_cast<double>(current[k] - previous[k]));
            }
            flux[static_cast<std::size_t>(f)] = sum;
            previous.swap(current);
        }

        const double maxFlux = flux.empty() ? 0.0 : *std::max_element(flux.begin(), flux.end());
        const double delta = 0.02 * maxFlux;
        const auto minIntervalFrames =
            static_cast<int>(std::ceil(minIntervalMs * 0.001 * audio.sampleRate / static_cast<double>(hop)));
        constexpr double silenceRms = 3.16e-4; // -70 dBFS

        nlohmann::json onsets = nlohmann::json::array();
        int lastOnset = -1 << 30;
        std::vector<double> neighbourhood;
        for (int f = 0; f < numFrames; ++f) {
            const double value = flux[static_cast<std::size_t>(f)];
            if (value <= 0.0 || frameRms[static_cast<std::size_t>(f)] < silenceRms)
                continue;
            bool isPeak = true;
            for (int d = -2; d <= 2 && isPeak; ++d) {
                const int g = f + d;
                if (d == 0 || g < 0 || g >= numFrames)
                    continue;
                if (flux[static_cast<std::size_t>(g)] > value || (d > 0 && flux[static_cast<std::size_t>(g)] == value))
                    isPeak = false;
            }
            if (!isPeak)
                continue;

            neighbourhood.clear();
            for (int g = std::max(0, f - medianRadius); g <= std::min(numFrames - 1, f + medianRadius); ++g)
                neighbourhood.push_back(flux[static_cast<std::size_t>(g)]);
            std::nth_element(neighbourhood.begin(), neighbourhood.begin() + static_cast<std::ptrdiff_t>(neighbourhood.size() / 2),
                             neighbourhood.end());
            const double threshold = neighbourhood[neighbourhood.size() / 2] * sensitivity + delta;
            if (value <= threshold || f - lastOnset < minIntervalFrames)
                continue;

            // Reject offsets: an abrupt stop (e.g. a stem ending on a non-zero
            // sample) splatters energy across the spectrum and produces positive
            // flux although the level falls. A real attack raises the frame level
            // relative to the frame one window earlier (silence before the file).
            const int lookback = frameSize / hop;
            const double prior = f >= lookback ? frameRms[static_cast<std::size_t>(f - lookback)] : 0.0;
            if (frameRms[static_cast<std::size_t>(f)] <= prior * 1.1)
                continue;
            lastOnset = f;

            const std::int64_t position = refine(mono, static_cast<std::int64_t>(f) * hop, frameSize, hop);
            onsets.push_back({{"frame", position},
                              {"timeSec", static_cast<double>(position) / audio.sampleRate},
                              {"strength", maxFlux > 0.0 ? value / maxFlux : 0.0}});
        }

        AnalysisResult result;
        result.data = {{"sampleRate", audio.sampleRate},
                       {"frameSize", frameSize},
                       {"hop", hop},
                       {"count", onsets.size()},
                       {"onsets", std::move(onsets)}};
        context.progress(1.0f);
        return result;
    }

private:
    // Finds the attack start inside the analysis window. Works on the first
    // difference (a +6 dB/oct high-pass), which emphasises the broadband attack
    // and suppresses sustained low-frequency content - drones, bass and the
    // bleed typical of Suno stems (~-40 dB at 55 Hz). Returns the first sample
    // whose short-term attack envelope reaches 25% of the window's peak.
    static std::int64_t refine(const std::vector<float>& x, std::int64_t frameStart, int frameSize, int hop)
    {
        const auto length = static_cast<std::int64_t>(x.size());
        const std::int64_t begin = std::max<std::int64_t>(1, frameStart - hop);
        const std::int64_t end = std::min<std::int64_t>(length, frameStart + frameSize);
        if (end <= begin)
            return std::clamp<std::int64_t>(frameStart, 0, std::max<std::int64_t>(0, length - 1));

        constexpr int smoothing = 16; // samples (~0.35 ms at 44.1 kHz)
        const auto count = static_cast<std::size_t>(end - begin);
        std::vector<float> envelope(count, 0.0f);
        float running = 0.0f;
        std::vector<float> diff(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto n = static_cast<std::size_t>(begin) + i;
            diff[i] = std::abs(x[n] - x[n - 1]);
        }
        // Forward moving average: envelope[i] = mean(diff[i .. i + smoothing)).
        for (std::size_t i = count; i-- > 0;) {
            running += diff[i];
            if (i + smoothing < count)
                running -= diff[i + smoothing];
            envelope[i] = running / static_cast<float>(smoothing);
        }

        const auto peak = std::max_element(envelope.begin(), envelope.end());
        const float target = 0.25f * *peak;
        for (auto it = envelope.begin(); it != peak; ++it)
            if (*it >= target)
                return begin + (it - envelope.begin());
        return begin + (peak - envelope.begin());
    }
};

} // namespace

std::shared_ptr<IAnalyzer> createTransientAnalyzer()
{
    return std::make_shared<TransientAnalyzer>();
}

} // namespace daw
