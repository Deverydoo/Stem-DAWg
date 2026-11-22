// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/ai/Analyzers.h>

#include "DspUtil.h"

#include <algorithm>
#include <cmath>

namespace daw {

namespace {

class LevelAnalyzer final : public IAnalyzer {
public:
    std::string id() const override { return kLevelAnalyzerId; }
    int version() const override { return 1; }
    std::string displayName() const override { return "Levels"; }

    Result<AnalysisResult> analyze(const AudioAsset& audio, const nlohmann::json& params, AnalysisContext& context) override
    {
        const double clipThreshold = params.value("clipThreshold", 0.999);
        const int channels = audio.numChannels();
        const std::int64_t frames = audio.numFrames();
        constexpr std::int64_t chunk = 1 << 16;

        nlohmann::json perChannel = nlohmann::json::array();
        double totalPower = 0.0;
        double overallPeak = 0.0;
        std::int64_t overallClipped = 0;

        for (int c = 0; c < channels; ++c) {
            const float* x = audio.channelData(c);
            double peak = 0.0, sumSquares = 0.0, sum = 0.0;
            std::int64_t clipped = 0;
            for (std::int64_t start = 0; start < frames; start += chunk) {
                if (context.cancel.isCancelled())
                    return makeError(ErrorCode::Cancelled, "level analysis cancelled");
                const std::int64_t end = std::min(frames, start + chunk);
                for (std::int64_t i = start; i < end; ++i) {
                    const double v = x[i];
                    const double a = std::abs(v);
                    peak = std::max(peak, a);
                    sumSquares += v * v;
                    sum += v;
                    if (a >= clipThreshold)
                        ++clipped;
                }
                context.progress((static_cast<float>(c) + static_cast<float>(end) / static_cast<float>(frames))
                                 / static_cast<float>(channels));
            }
            const double meanSquare = frames > 0 ? sumSquares / static_cast<double>(frames) : 0.0;
            const double rms = std::sqrt(meanSquare);
            perChannel.push_back({{"peakDbfs", detail::amplitudeToDb(peak)},
                                  {"rmsDbfs", detail::amplitudeToDb(rms)},
                                  {"crestDb", rms > 0.0 ? detail::amplitudeToDb(peak) - detail::amplitudeToDb(rms) : 0.0},
                                  {"dcOffset", frames > 0 ? sum / static_cast<double>(frames) : 0.0},
                                  {"clippedSamples", clipped}});
            totalPower += meanSquare;
            overallPeak = std::max(overallPeak, peak);
            overallClipped += clipped;
        }

        nlohmann::json correlation = nullptr;
        if (channels == 2) {
            const float* l = audio.channelData(0);
            const float* r = audio.channelData(1);
            double lr = 0.0, ll = 0.0, rr = 0.0;
            for (std::int64_t i = 0; i < frames; ++i) {
                lr += static_cast<double>(l[i]) * r[i];
                ll += static_cast<double>(l[i]) * l[i];
                rr += static_cast<double>(r[i]) * r[i];
            }
            const double denom = std::sqrt(ll * rr);
            correlation = denom > 0.0 ? lr / denom : 0.0;
        }

        AnalysisResult result;
        result.data = {{"sampleRate", audio.sampleRate},
                       {"frames", frames},
                       {"channels", perChannel},
                       {"overall",
                        {{"peakDbfs", detail::amplitudeToDb(overallPeak)},
                         {"rmsDbfs", detail::powerToDb(channels > 0 ? totalPower / channels : 0.0)},
                         {"clippedSamples", overallClipped},
                         {"stereoCorrelation", correlation}}}};
        context.progress(1.0f);
        return result;
    }
};

} // namespace

std::shared_ptr<IAnalyzer> createLevelAnalyzer()
{
    return std::make_shared<LevelAnalyzer>();
}

} // namespace daw
