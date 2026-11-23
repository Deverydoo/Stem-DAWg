// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/session/NoiseTools.h>

#include <daw/dsp/Fft.h>
#include <daw/dsp/SpectralDenoiser.h>
#include <daw/engine/AudioEngine.h>
#include <daw/session/Renderer.h>
#include <daw/session/SnapshotBuilder.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <numbers>
#include <vector>

namespace daw {

namespace {

constexpr double kSilentBinPower = 1e-14; // mean power per bin below this is digital silence
constexpr int kHumFftSize = 16384;        // ~2.9 Hz bins at 48 kHz: 50 and 60 Hz well apart

struct QuietSpectrum {
    std::vector<double> power; // mean power per bin of the quiet frames
    double seconds = 0.0;
};

/// The track alone, at unity gain, with only the chain stages before the one being set up.
ProjectModel inputOfStage(const ProjectModel& model, const Track& track, bool withHum)
{
    ProjectModel single = model;
    Track copy = track;
    copy.gainDb = 0.0;
    copy.pan = 0.0;
    copy.muted = false;
    copy.soloed = false;
    TrackCleanup chain;
    if (!track.cleanup.bypassed) {
        chain.dcRemoval = track.cleanup.dcRemoval;
        chain.highPass = track.cleanup.highPass;
        if (withHum)
            chain.hum = track.cleanup.hum;
    }
    copy.cleanup = chain;
    single.tracks = {copy};
    single.masterGainDb = 0.0;
    return single;
}

/// Streams the stage input over `range` in frames of `fftSize` samples at
/// 75 % overlap and calls `onFrame(left, right)` with each complete frame
/// (not windowed). Frames lie wholly inside the range.
template <typename OnFrame>
Status streamFrames(const ProjectModel& single, const MediaPool& media, int fftSize, FrameRange range,
                    const CancellationToken& cancel, OnFrame onFrame)
{
    const double rate = single.sampleRate;
    const int hop = fftSize / 4;
    auto engine = std::make_unique<AudioEngine>();
    engine->prepare({.sampleRate = rate, .maxBlockSize = hop, .numOutputChannels = 2, .meterLoudness = false, .maxCleanupTracks = 1});
    engine->publishSnapshot(buildPlaybackSnapshot(single, media, rate, 1));
    engine->setStopAtEnd(false);
    engine->seek(range.start); // the engine primes the chain from the audio before the range
    engine->play();

    std::vector<float> left(static_cast<std::size_t>(fftSize), 0.0f), right(static_cast<std::size_t>(fftSize), 0.0f);
    const std::int64_t total = range.length();
    std::int64_t done = 0;
    int filled = 0;
    while (done + hop <= total) {
        if (cancel.isCancelled())
            return makeError(ErrorCode::Cancelled, "noise measurement cancelled");
        std::copy(left.begin() + hop, left.end(), left.begin());
        std::copy(right.begin() + hop, right.end(), right.begin());
        float* outs[2] = {left.data() + fftSize - hop, right.data() + fftSize - hop};
        engine->process(outs, 2, hop);
        done += hop;
        filled = std::min(filled + hop, fftSize);
        if (filled == fftSize)
            onFrame(left.data(), right.data());
    }
    return Status::success();
}

/// The mean spectrum of the track's quiet frames: the quietest `quietFraction`
/// of the non-silent ones, or every non-silent frame of `region`.
Result<QuietSpectrum> quietSpectrum(const ProjectModel& model, const MediaPool& pool, const TrackId& trackId,
                                    int fftSize, bool withHum, double quietFraction,
                                    const std::optional<FrameRange>& region, const CancellationToken& cancel)
{
    const Track* track = model.findTrack(trackId);
    if (track == nullptr)
        return makeError(ErrorCode::NotFound, "no such track");
    if (!(model.sampleRate > 0.0))
        return makeError(ErrorCode::InvalidArgument, "project has no sample rate");
    const ProjectModel single = inputOfStage(model, *track, withHum);
    if (single.contentEnd() <= 0)
        return makeError(ErrorCode::InvalidArgument, "the track has no audio");
    FrameRange range{0, single.contentEnd()};
    if (region) {
        range.start = std::clamp<std::int64_t>(region->start, 0, single.contentEnd());
        range.end = std::clamp<std::int64_t>(region->end, range.start, single.contentEnd());
        if (range.length() < fftSize)
            return makeError(ErrorCode::InvalidArgument, "the selection is too short to measure the noise in");
        quietFraction = 1.0;
    }
    auto prepared = prepareMediaAtRate(single, pool, single.sampleRate, cancel);
    if (!prepared)
        return prepared.error();
    const MediaPool& media = **prepared;

    // Square-root Hann, as the denoiser windows its frames.
    std::vector<float> window(static_cast<std::size_t>(fftSize));
    std::vector<float> windowSquared(static_cast<std::size_t>(fftSize));
    for (int i = 0; i < fftSize; ++i) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / fftSize);
        window[static_cast<std::size_t>(i)] = static_cast<float>(std::sqrt(hann));
        windowSquared[static_cast<std::size_t>(i)] = static_cast<float>(hann);
    }
    const auto bins = static_cast<double>(fftSize / 2 + 1);

    // Pass 1: the level of every frame, as mean power per bin. By Parseval's
    // theorem this is the windowed energy in the time domain (no FFT needed):
    // sum over bins of SpectralDenoiser-style powers ~= N / 4 * sum(w^2 (l^2 + r^2)).
    std::vector<float> levels;
    auto status = streamFrames(single, media, fftSize, range, cancel, [&](const float* l, const float* r) {
        double energy = 0.0;
        for (int i = 0; i < fftSize; ++i)
            energy += windowSquared[static_cast<std::size_t>(i)] * (static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i]);
        levels.push_back(static_cast<float>(0.25 * fftSize * energy / bins));
    });
    if (!status)
        return status.error();
    std::vector<float> sounding;
    for (float level : levels)
        if (level > kSilentBinPower)
            sounding.push_back(level);
    if (sounding.empty())
        return makeError(ErrorCode::InvalidArgument, region ? "the selection is silent: there is no noise to measure"
                                                            : "the track is silent: there is no noise to measure");
    const auto index = static_cast<std::size_t>(std::clamp(quietFraction, 0.0, 1.0) * static_cast<double>(sounding.size() - 1));
    std::nth_element(sounding.begin(), sounding.begin() + static_cast<std::ptrdiff_t>(index), sounding.end());
    const float threshold = sounding[index];

    // Pass 2: the mean spectrum of the quiet frames (only they are transformed).
    Fft fft(fftSize);
    std::vector<std::complex<float>> z(static_cast<std::size_t>(fftSize));
    QuietSpectrum result;
    result.power.assign(static_cast<std::size_t>(fftSize / 2 + 1), 0.0);
    std::size_t frame = 0, used = 0;
    status = streamFrames(single, media, fftSize, range, cancel, [&](const float* l, const float* r) {
        const float level = frame < levels.size() ? levels[frame] : 0.0f;
        ++frame;
        if (level <= kSilentBinPower || level > threshold)
            return;
        for (int i = 0; i < fftSize; ++i)
            z[static_cast<std::size_t>(i)] = {l[i] * window[static_cast<std::size_t>(i)], r[i] * window[static_cast<std::size_t>(i)]};
        fft.forward(z.data());
        for (int k = 0; k <= fftSize / 2; ++k) // powers exactly as SpectralDenoiser::framePower
            result.power[static_cast<std::size_t>(k)] +=
                0.25 * (std::norm(z[static_cast<std::size_t>(k)]) + std::norm(z[static_cast<std::size_t>((fftSize - k) % fftSize)]));
        ++used;
    });
    if (!status)
        return status.error();
    for (double& p : result.power)
        p /= static_cast<double>(std::max<std::size_t>(used, 1));
    result.seconds = static_cast<double>(used) * (fftSize / 4) / single.sampleRate;
    return result;
}

} // namespace

Result<NoiseProfile> learnNoiseProfile(const ProjectModel& model, const MediaPool& pool, const TrackId& track,
                                       const NoiseLearnOptions& options, const CancellationToken& cancel)
{
    auto quiet = quietSpectrum(model, pool, track, SpectralDenoiser::kFftSize, true, options.quietFraction,
                               options.region, cancel);
    if (!quiet)
        return quiet.error();
    if (quiet->seconds < options.minSeconds)
        return makeError(ErrorCode::InvalidArgument, options.region
                                                         ? "the selection holds too little noise to learn from: select a longer stretch"
                                                         : "the track has too little quiet audio to learn the noise from");
    NoiseProfile profile;
    profile.sampleRate = model.sampleRate;
    profile.fftSize = SpectralDenoiser::kFftSize;
    profile.seconds = std::round(quiet->seconds * 100.0) / 100.0;
    profile.fromSelection = options.region.has_value();
    profile.powerDb.reserve(quiet->power.size());
    for (double p : quiet->power) // 0.01 dB steps: what the project file stores
        profile.powerDb.push_back(static_cast<float>(std::round(10.0 * std::log10(std::max(p, 1e-30)) * 100.0) / 100.0));
    return profile;
}

Result<std::optional<HumDetection>> detectHum(const ProjectModel& model, const MediaPool& pool, const TrackId& track,
                                              const CancellationToken& cancel, const std::optional<FrameRange>& region)
{
    auto quiet = quietSpectrum(model, pool, track, kHumFftSize, false, 0.25, region, cancel);
    if (!quiet)
        return quiet.error();
    const std::vector<double>& power = quiet->power;
    const double binHz = model.sampleRate / kHumFftSize;
    const auto bins = static_cast<int>(power.size());

    // Prominence of harmonic h: its peak over the median of the spectrum
    // 10-35 Hz to either side. Mains hum sits on exact multiples of 50 or
    // 60 Hz (the grid drifts by a few tenths of a hertz at most), while a
    // musical note rarely does (G1 is 49.0 Hz, B-flat 1 58.3 Hz): the peak's
    // interpolated frequency must match within 0.5 Hz + 0.25 Hz per harmonic.
    const auto prominence = [&](double mains, int h) {
        const double hz = mains * h;
        const int centre = static_cast<int>(std::lround(hz / binHz));
        const int inner = static_cast<int>(std::ceil(10.0 / binHz));
        const int outer = static_cast<int>(std::ceil(35.0 / binHz));
        if (centre + outer >= bins || centre - outer < 1)
            return 0.0;
        int top = centre - 1;
        for (int k = centre; k <= centre + 1; ++k)
            if (power[static_cast<std::size_t>(k)] > power[static_cast<std::size_t>(top)])
                top = k;
        const double peak = power[static_cast<std::size_t>(top)];
        const auto db = [&](int k) { return 10.0 * std::log10(std::max(power[static_cast<std::size_t>(k)], 1e-30)); };
        const double a = db(top - 1), b = db(top), c = db(top + 1);
        const double curvature = a - 2.0 * b + c;
        const double offset = curvature < 0.0 ? std::clamp(0.5 * (a - c) / curvature, -0.5, 0.5) : 0.0;
        if (std::abs((top + offset) * binHz - hz) > 0.5 + 0.25 * h)
            return 0.0; // a note near the harmonic, not the harmonic
        std::vector<double> around;
        for (int d = inner; d <= outer; ++d) {
            around.push_back(power[static_cast<std::size_t>(centre - d)]);
            around.push_back(power[static_cast<std::size_t>(centre + d)]);
        }
        std::nth_element(around.begin(), around.begin() + static_cast<std::ptrdiff_t>(around.size() / 2), around.end());
        const double median = around[around.size() / 2];
        return median > 0.0 && peak > 0.0 ? 10.0 * std::log10(peak / median) : 0.0;
    };

    std::optional<HumDetection> best;
    for (const double mains : {50.0, 60.0}) {
        double strongest = 0.0;
        for (int h = 1; h <= 3; ++h)
            strongest = std::max(strongest, prominence(mains, h));
        if (strongest < kHumDetectDb || (best && strongest <= best->prominenceDb))
            continue;
        int harmonics = 1;
        for (int h = 1; h <= 8; ++h)
            if (prominence(mains, h) >= kHumHarmonicDb)
                harmonics = h;
        best = HumDetection{mains, harmonics, std::round(strongest * 10.0) / 10.0};
    }
    return best;
}

} // namespace daw
