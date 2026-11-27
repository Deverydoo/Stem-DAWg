// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/ai/NoteTranscriber.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <string>
#include <utility>

namespace daw {

extern const unsigned char kBasicPitchModel[];
extern const std::size_t kBasicPitchModelSize;

namespace {

constexpr const char* kModelId = "basic-pitch.icassp-2022";
constexpr int kBatch = 8;          // windows per inference
constexpr int kEnergyTolerance = 11; // frames below the threshold that end a note

// Frame f of a window sits at sample f * kHop of the window's audio (the
// model's spectrogram is centred). Measured on notes rendered through a
// SoundFont, starts then land within a few milliseconds of the truth (bass,
// piano) or up to 20 ms late (slow synth attacks), so no offset is added.
constexpr double kAlignmentSeconds = 0.0;

} // namespace

Result<std::shared_ptr<NoteTranscriber>> NoteTranscriber::load(std::shared_ptr<InferenceBackend> backend)
{
    if (!backend)
        return makeError(ErrorCode::Unavailable, "no inference backend");
    std::shared_ptr<NoteTranscriber> transcriber(new NoteTranscriber());
    transcriber->backend_ = backend;
    auto info = backend->isLoaded(kModelId)
                    ? backend->info(kModelId)
                    : backend->load(ModelDescriptor{kModelId, "icassp-2022", "note-transcription", {},
                                                    std::vector<std::uint8_t>(kBasicPitchModel, kBasicPitchModel + kBasicPitchModelSize),
                                                    {}});
    if (!info)
        return info.error();
    if (info->inputNames.size() != 1 || info->outputNames.size() != 3)
        return makeError(ErrorCode::InvalidArgument, "unexpected Basic Pitch model graph");
    transcriber->input_ = info->inputNames.front();
    // The exported graph names its outputs StatefulPartitionedCall:0
    // (contour), :1 (note) and :2 (onset).
    for (std::size_t i = 0; i < info->outputNames.size(); ++i) {
        const std::string& name = info->outputNames[i];
        if (name.ends_with(":1"))
            transcriber->noteOutput_ = i;
        else if (name.ends_with(":2"))
            transcriber->onsetOutput_ = i;
    }
    if (transcriber->noteOutput_ == transcriber->onsetOutput_)
        return makeError(ErrorCode::InvalidArgument, "unexpected Basic Pitch output names");
    return transcriber;
}

Result<NoteActivations> NoteTranscriber::analyse(const float* mono, std::size_t length, const CancellationToken& cancel,
                                                 const std::function<void(double)>& progress) const
{
    NoteActivations out;
    if (length == 0)
        return out;
    // The audio is padded with half an overlap of silence in front, then cut
    // into windows a window less an overlap apart; each window keeps its
    // middle frames (half an overlap dropped at each end).
    constexpr int overlapSamples = kOverlapFrames * kHop;
    constexpr int pad = overlapSamples / 2;
    constexpr int stride = kWindowSamples - overlapSamples;
    constexpr int keepFrom = kOverlapFrames / 2;
    constexpr int keepTo = kWindowFrames - kOverlapFrames / 2;
    const auto padded = static_cast<std::int64_t>(length) + pad;
    const auto windows = static_cast<int>((padded + stride - 1) / stride);
    const double duration = static_cast<double>(length) / kSampleRate;

    const auto sampleAt = [&](std::int64_t i) {
        const std::int64_t j = i - pad;
        return j >= 0 && j < static_cast<std::int64_t>(length) ? mono[j] : 0.0f;
    };

    out.frames.reserve(static_cast<std::size_t>(windows) * (keepTo - keepFrom) * kPianoKeys);
    out.onsets.reserve(out.frames.capacity());
    out.times.reserve(static_cast<std::size_t>(windows) * (keepTo - keepFrom));
    for (int first = 0; first < windows; first += kBatch) {
        if (cancel.isCancelled())
            return makeError(ErrorCode::Cancelled, "note transcription cancelled");
        const int count = std::min(kBatch, windows - first);
        TensorF input;
        input.shape = {count, kWindowSamples, 1};
        input.data.resize(static_cast<std::size_t>(count) * kWindowSamples);
        for (int w = 0; w < count; ++w) {
            const std::int64_t start = static_cast<std::int64_t>(first + w) * stride;
            float* dst = input.data.data() + static_cast<std::size_t>(w) * kWindowSamples;
            for (int i = 0; i < kWindowSamples; ++i)
                dst[i] = sampleAt(start + i);
        }
        auto result = backend_->run(kModelId, {{input_, std::move(input)}}, cancel);
        if (!result)
            return result.error();
        const TensorF& note = (*result)[noteOutput_];
        const TensorF& onset = (*result)[onsetOutput_];
        const std::size_t expected = static_cast<std::size_t>(count) * kWindowFrames * kPianoKeys;
        if (note.data.size() != expected || onset.data.size() != expected)
            return makeError(ErrorCode::InvalidArgument, "unexpected Basic Pitch output shape");
        for (int w = 0; w < count; ++w) {
            const std::int64_t start = static_cast<std::int64_t>(first + w) * stride - pad;
            for (int f = keepFrom; f < keepTo; ++f) {
                const double t = static_cast<double>(start + static_cast<std::int64_t>(f) * kHop) / kSampleRate + kAlignmentSeconds;
                if (t < 0.0 || t >= duration)
                    continue;
                const std::size_t row = (static_cast<std::size_t>(w) * kWindowFrames + static_cast<std::size_t>(f)) * kPianoKeys;
                out.frames.insert(out.frames.end(), note.data.begin() + static_cast<std::ptrdiff_t>(row),
                                  note.data.begin() + static_cast<std::ptrdiff_t>(row + kPianoKeys));
                out.onsets.insert(out.onsets.end(), onset.data.begin() + static_cast<std::ptrdiff_t>(row),
                                  onset.data.begin() + static_cast<std::ptrdiff_t>(row + kPianoKeys));
                out.times.push_back(t);
            }
        }
        if (progress)
            progress(static_cast<double>(first + count) / windows);
    }
    return out;
}

std::vector<PitchedNote> decodeNotes(const NoteActivations& act, const NoteDecodeParams& params)
{
    std::vector<PitchedNote> notes;
    const auto n = static_cast<int>(act.count());
    constexpr int keys = kPianoKeys;
    if (n < 3)
        return notes;
    const int minFrames = static_cast<int>(std::lround(params.minNoteSeconds * NoteTranscriber::kFrameRate));
    const int lowest = std::clamp(params.lowestKey - kPianoLowestKey, 0, keys);
    const int highest = std::clamp(params.highestKey - kPianoLowestKey + 1, 0, keys);
    const auto at = [](int t, int k) { return static_cast<std::size_t>(t) * keys + static_cast<std::size_t>(k); };

    std::vector<float> frames = act.frames;
    std::vector<float> onsets = act.onsets;
    for (int t = 0; t < n; ++t)
        for (int k = 0; k < keys; ++k)
            if (k < lowest || k >= highest)
                frames[at(t, k)] = onsets[at(t, k)] = 0.0f;

    // Onsets inferred from jumps in the note activation (over one and two
    // frames, the smaller), scaled to the onsets' maximum.
    if (params.inferOnsets) {
        std::vector<float> diff(frames.size(), 0.0f);
        float maxDiff = 0.0f;
        float maxOnset = 0.0f;
        for (int t = 0; t < n; ++t)
            for (int k = 0; k < keys; ++k) {
                maxOnset = std::max(maxOnset, onsets[at(t, k)]);
                if (t < 2)
                    continue;
                const float d = std::min(frames[at(t, k)] - frames[at(t - 1, k)], frames[at(t, k)] - frames[at(t - 2, k)]);
                diff[at(t, k)] = std::max(0.0f, d);
                maxDiff = std::max(maxDiff, diff[at(t, k)]);
            }
        if (maxDiff > 0.0f)
            for (std::size_t i = 0; i < onsets.size(); ++i)
                onsets[i] = std::max(onsets[i], maxOnset * diff[i] / maxDiff);
    }

    // Onset peaks (strict local maxima in time) at or above the threshold,
    // latest first.
    std::vector<std::pair<int, int>> starts;
    for (int t = n - 2; t >= 1; --t)
        for (int k = keys - 1; k >= 0; --k) {
            const float v = onsets[at(t, k)];
            if (v >= params.onsetThreshold && v > onsets[at(t - 1, k)] && v > onsets[at(t + 1, k)])
                starts.emplace_back(t, k);
        }

    std::vector<float> remaining = frames;
    const auto clearAround = [&](int t, int k) {
        remaining[at(t, k)] = 0.0f;
        if (k + 1 < keys)
            remaining[at(t, k + 1)] = 0.0f;
        if (k > 0)
            remaining[at(t, k - 1)] = 0.0f;
    };
    const auto addNote = [&](int from, int to, int k) {
        double sum = 0.0;
        for (int t = from; t < to; ++t)
            sum += frames[at(t, k)];
        const auto last = static_cast<std::size_t>(std::min(to, n - 1));
        notes.push_back({act.times[static_cast<std::size_t>(from)], act.times[last], k + kPianoLowestKey,
                         static_cast<float>(sum / std::max(1, to - from))});
    };

    for (const auto& [start, k] : starts) {
        if (start >= n - 1)
            continue;
        int i = start + 1;
        int below = 0;
        while (i < n - 1 && below < kEnergyTolerance) {
            below = remaining[at(i, k)] < params.frameThreshold ? below + 1 : 0;
            ++i;
        }
        i -= below;
        if (i - start <= minFrames)
            continue;
        for (int t = start; t < i; ++t)
            clearAround(t, k);
        addNote(start, i, k);
    }

    if (params.melodia) {
        // Repeatedly take the strongest remaining activation and follow it
        // both ways. Activations only ever drop to zero, so a max-heap with
        // stale entries skipped finds the same maximum as a full scan (ties
        // to the earliest, as numpy's argmax).
        using Entry = std::pair<float, std::int64_t>; // value, -flat index
        std::priority_queue<Entry> heap;
        for (std::size_t i = 0; i < remaining.size(); ++i)
            if (remaining[i] > params.frameThreshold)
                heap.emplace(remaining[i], -static_cast<std::int64_t>(i));
        while (!heap.empty()) {
            const auto [value, negIndex] = heap.top();
            heap.pop();
            const auto flat = static_cast<std::size_t>(-negIndex);
            if (remaining[flat] != value)
                continue; // cleared since
            const int mid = static_cast<int>(flat / keys);
            const int k = static_cast<int>(flat % keys);
            remaining[flat] = 0.0f;

            int i = mid + 1;
            int below = 0;
            while (i < n - 1 && below < kEnergyTolerance) {
                below = remaining[at(i, k)] < params.frameThreshold ? below + 1 : 0;
                clearAround(i, k);
                ++i;
            }
            const int end = i - 1 - below;

            i = mid - 1;
            below = 0;
            while (i > 0 && below < kEnergyTolerance) {
                below = remaining[at(i, k)] < params.frameThreshold ? below + 1 : 0;
                clearAround(i, k);
                --i;
            }
            const int begin = i + 1 + below;
            if (end - begin <= minFrames)
                continue;
            addNote(begin, end, k);
        }
    }
    return notes;
}

} // namespace daw
