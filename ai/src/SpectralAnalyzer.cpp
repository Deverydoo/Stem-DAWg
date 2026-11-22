// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/ai/Analyzers.h>
#include <daw/ai/Fft.h>

#include "DspUtil.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace daw {

namespace {

struct Band {
    const char* name;
    double lowHz;
    double highHz; // < 0 means Nyquist
};

constexpr std::array<Band, 8> kBands{{
    {"sub", 20.0, 60.0},
    {"bass", 60.0, 250.0},
    {"lowMid", 250.0, 500.0},
    {"mid", 500.0, 2000.0},
    {"upperMid", 2000.0, 4000.0},
    {"presence", 4000.0, 6000.0},
    {"brilliance", 6000.0, 12000.0},
    {"air", 12000.0, -1.0},
}};

// Long-term average spectrum summarised into industrial-mix-relevant bands.
// "harshnessRatio" (2-5 kHz share) is the first input to Suno guitar
// harshness detection; the learned detector in Milestone 6 refines it.
class SpectralAnalyzer final : public IAnalyzer {
public:
    std::string id() const override { return kSpectralAnalyzerId; }
    int version() const override { return 1; }
    std::string displayName() const override { return "Spectral balance"; }

    Result<AnalysisResult> analyze(const AudioAsset& audio, const nlohmann::json& params, AnalysisContext& context) override
    {
        const int fftSize = params.value("fftSize", 4096);
        if (fftSize < 256 || (fftSize & (fftSize - 1)) != 0)
            return makeError(ErrorCode::InvalidArgument, "fftSize must be a power of two >= 256");
        const int hop = fftSize / 2;
        const double nyquist = audio.sampleRate / 2.0;

        const std::vector<float> mono = detail::monoMix(audio);
        const auto length = static_cast<std::int64_t>(mono.size());
        const Fft fft(fftSize);
        const std::vector<float> window = hannWindow(fftSize);
        std::vector<float> frame(static_cast<std::size_t>(fftSize)), power;
        std::vector<std::complex<float>> scratch;
        std::vector<double> average(static_cast<std::size_t>(fftSize / 2 + 1), 0.0);

        const auto frames = static_cast<int>(std::max<std::int64_t>(1, (length + hop - 1) / hop));
        for (int f = 0; f < frames; ++f) {
            if ((f & 0x3F) == 0) {
                if (context.cancel.isCancelled())
                    return makeError(ErrorCode::Cancelled, "spectral analysis cancelled");
                context.progress(static_cast<float>(f) / static_cast<float>(frames));
            }
            const std::int64_t start = static_cast<std::int64_t>(f) * hop;
            for (int i = 0; i < fftSize; ++i) {
                const std::int64_t index = start + i;
                frame[static_cast<std::size_t>(i)] =
                    (index < length ? mono[static_cast<std::size_t>(index)] : 0.0f) * window[static_cast<std::size_t>(i)];
            }
            fft.powerSpectrum(frame.data(), power, scratch);
            for (std::size_t k = 0; k < power.size(); ++k)
                average[k] += power[k];
        }

        const double binHz = audio.sampleRate / fftSize;
        double total = 0.0, weighted = 0.0, harsh = 0.0;
        std::array<double, kBands.size()> bandPower{};
        for (std::size_t k = 1; k < average.size(); ++k) {
            const double hz = static_cast<double>(k) * binHz;
            if (hz < 20.0)
                continue;
            const double p = average[k];
            total += p;
            weighted += p * hz;
            if (hz >= 2000.0 && hz < 5000.0)
                harsh += p;
            for (std::size_t b = 0; b < kBands.size(); ++b) {
                const double high = kBands[b].highHz < 0.0 ? nyquist + 1.0 : kBands[b].highHz;
                if (hz >= kBands[b].lowHz && hz < high) {
                    bandPower[b] += p;
                    break;
                }
            }
        }

        nlohmann::json bands = nlohmann::json::array();
        const double norm = static_cast<double>(frames) * fftSize * fftSize;
        for (std::size_t b = 0; b < kBands.size(); ++b) {
            bands.push_back({{"name", kBands[b].name},
                             {"lowHz", kBands[b].lowHz},
                             {"highHz", kBands[b].highHz < 0.0 ? nyquist : kBands[b].highHz},
                             {"energyDb", detail::powerToDb(bandPower[b] / norm)},
                             {"fraction", total > 0.0 ? bandPower[b] / total : 0.0}});
        }

        AnalysisResult result;
        result.data = {{"sampleRate", audio.sampleRate},
                       {"fftSize", fftSize},
                       {"bands", std::move(bands)},
                       {"centroidHz", total > 0.0 ? weighted / total : 0.0},
                       {"harshnessRatio", total > 0.0 ? harsh / total : 0.0}};
        context.progress(1.0f);
        return result;
    }
};

} // namespace

std::shared_ptr<IAnalyzer> createSpectralAnalyzer()
{
    return std::make_shared<SpectralAnalyzer>();
}

} // namespace daw
