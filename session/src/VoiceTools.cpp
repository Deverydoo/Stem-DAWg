// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/session/VoiceTools.h>

#include "ProcessedMedia.h"

#include <daw/core/Files.h>
#include <daw/core/Sha256.h>
#include <daw/media/TarGzArchive.h>
#include <daw/media/Midi.h>
#include <daw/media/Resampler.h>
#include <daw/project/ProjectStore.h>
#include <daw/session/ProjectOperations.h>

#include <algorithm>
#include <cmath>

namespace fs = std::filesystem;

namespace daw {

using detail::blockPowers;
using detail::levelChange;

Result<VoiceDenoiseOutcome> denoiseVoiceMedia(const fs::path& projectRoot, const MediaRef& original, const AudioAsset& audio,
                                              const VoiceDenoiser& denoiser, double reductionDb, double engineRate,
                                              const CancellationToken& cancel, const std::function<void(double)>& progress)
{
    if (isMidiFile(utf8ToPath(original.path)))
        return makeError(ErrorCode::InvalidArgument, "MIDI tracks are synthesised: there is no recorded noise to remove");
    if (audio.numFrames() <= 0 || audio.numChannels() <= 0)
        return makeError(ErrorCode::InvalidArgument, "the track has no audio");

    // The model runs at 48 kHz; the copy keeps the original's rate.
    std::shared_ptr<const AudioAsset> input;
    if (audio.sampleRate != VoiceDenoiser::kSampleRate) {
        auto resampled = resampleAudio(audio, VoiceDenoiser::kSampleRate, {}, cancel);
        if (!resampled)
            return resampled.error();
        input = *resampled;
    }
    const AudioAsset& in = input ? *input : audio;
    auto processed = AudioAsset::create(in.numChannels(), in.numFrames(), VoiceDenoiser::kSampleRate);
    VoiceDenoiseOptions options;
    options.reductionDb = reductionDb;
    const int channels = in.numChannels();
    for (int c = 0; c < channels; ++c) {
        auto out = denoiser.process(in.channelData(c), static_cast<std::size_t>(in.numFrames()), options, cancel, [&](double p) {
            if (progress)
                progress(0.95 * (c + p) / channels);
        });
        if (!out)
            return out.error();
        processed->channels[static_cast<std::size_t>(c)] = std::move(*out);
    }
    std::shared_ptr<const AudioAsset> result = processed;
    if (audio.sampleRate != VoiceDenoiser::kSampleRate) {
        auto back = resampleAudio(*processed, audio.sampleRate, {}, cancel);
        if (!back)
            return back.error();
        result = *back;
    }
    if (result->numFrames() != audio.numFrames()) { // resampling round trips can differ by a frame
        auto exact = AudioAsset::create(result->numChannels(), audio.numFrames(), audio.sampleRate);
        for (int c = 0; c < result->numChannels(); ++c)
            std::copy_n(result->channels[static_cast<std::size_t>(c)].begin(),
                        std::min(result->numFrames(), audio.numFrames()), exact->channels[static_cast<std::size_t>(c)].begin());
        result = exact;
    }

    VoiceDenoiseOutcome outcome;
    const auto before = blockPowers(audio);
    const auto after = blockPowers(*result);
    outcome.voiceChangeDb = levelChange(before, after, 0.5, 1.0);
    outcome.noiseChangeDb = levelChange(before, after, 0.0, 0.1);

    auto copy = detail::writeProcessedCopy(projectRoot, original, *result, "voice", kVoiceDenoiseProcess, reductionDb, engineRate,
                                           cancel);
    if (!copy)
        return copy.error();
    outcome.media = std::move(copy->media);
    outcome.loaded = std::move(copy->loaded);
    if (progress)
        progress(1.0);
    return outcome;
}

fs::path voiceModelDirectory()
{
    return appDataDirectory() / "Models" / kDeepFilterNet3.folderName;
}

bool isVoiceModelInstalled(const fs::path& directory)
{
    std::error_code ec;
    for (const VoiceModelFile& file : kDeepFilterNet3.files)
        if (fs::file_size(directory / file.name, ec) != file.bytes || ec)
            return false;
    return true;
}

Status installVoiceModel(const fs::path& archive, const fs::path& directory, const CancellationToken& cancel)
{
    auto hash = sha256File(archive);
    if (!hash)
        return hash.error();
    if (*hash != kDeepFilterNet3.archiveSha256)
        return makeError(ErrorCode::Corrupt, "the downloaded voice model is not the expected file (SHA-256 mismatch)");
    std::vector<std::string> names;
    for (const VoiceModelFile& file : kDeepFilterNet3.files)
        names.emplace_back(file.name);
    if (auto status = extractTarGzFiles(archive, names, directory, std::size_t{64} << 20, cancel); !status)
        return status;
    for (const VoiceModelFile& file : kDeepFilterNet3.files) {
        auto fileHash = sha256File(directory / file.name);
        if (!fileHash || *fileHash != file.sha256) {
            std::error_code ec;
            for (const VoiceModelFile& f : kDeepFilterNet3.files)
                fs::remove(directory / f.name, ec);
            return makeError(ErrorCode::Corrupt, std::string("the voice model file ") + file.name + " is damaged");
        }
    }
    return Status::success();
}

const MediaRef* originalMediaOf(const ProjectModel& model, const MediaId& media)
{
    const MediaRef* ref = model.findMedia(media);
    for (int depth = 0; ref != nullptr && ref->derivation && depth < 16; ++depth) {
        const MediaRef* source = model.findMedia(ref->derivation->source);
        if (source == nullptr)
            break;
        ref = source;
    }
    return ref;
}

std::vector<MediaRef> trackOriginalMedia(const ProjectModel& model, const TrackId& trackId)
{
    std::vector<MediaRef> out;
    const Track* track = model.findTrack(trackId);
    if (track == nullptr)
        return out;
    for (const Clip& clip : track->clips)
        if (const MediaRef* original = originalMediaOf(model, clip.media))
            if (std::none_of(out.begin(), out.end(), [&](const MediaRef& m) { return m.id == original->id; }))
                out.push_back(*original);
    return out;
}

std::optional<MediaDerivation> trackProcessing(const ProjectModel& model, const TrackId& trackId)
{
    const Track* track = model.findTrack(trackId);
    if (track == nullptr || track->clips.empty())
        return std::nullopt;
    std::optional<MediaDerivation> found;
    for (const Clip& clip : track->clips) {
        const MediaRef* media = model.findMedia(clip.media);
        if (media == nullptr || !media->derivation || (found && media->derivation->process != found->process))
            return std::nullopt;
        found = media->derivation;
    }
    return found;
}

std::optional<MediaDerivation> trackVoiceDenoise(const ProjectModel& model, const TrackId& trackId)
{
    auto processing = trackProcessing(model, trackId);
    if (processing && processing->process != kVoiceDenoiseProcess)
        return std::nullopt;
    return processing;
}

bool applyProcessedMedia(ProjectModel& model, const TrackId& trackId, const std::vector<MediaRef>& processed)
{
    Track* track = model.findTrack(trackId);
    if (track == nullptr || processed.empty())
        return false;
    bool changed = false;
    for (const MediaRef& copy : processed) {
        if (!copy.derivation)
            continue;
        if (model.findMedia(copy.id) == nullptr)
            model.media.push_back(copy);
        for (Clip& clip : track->clips) {
            const MediaRef* original = originalMediaOf(model, clip.media);
            if (original != nullptr && original->id == copy.derivation->source && clip.media != copy.id) {
                clip.media = copy.id;
                changed = true;
            }
        }
    }
    pruneUnusedMedia(model);
    return changed;
}

std::size_t fallBackFromMissingProcessedMedia(ProjectModel& model, const fs::path& projectRoot)
{
    std::size_t changed = 0;
    for (Track& track : model.tracks)
        for (Clip& clip : track.clips) {
            const MediaRef* media = model.findMedia(clip.media);
            if (media == nullptr || !media->derivation)
                continue;
            std::error_code ec;
            if (fs::is_regular_file(ProjectStore::resolveMediaPath(projectRoot, *media), ec))
                continue;
            const MediaRef* original = originalMediaOf(model, clip.media);
            if (original != nullptr && original->id != clip.media) {
                clip.media = original->id;
                ++changed;
            }
        }
    if (changed > 0)
        pruneUnusedMedia(model);
    return changed;
}

bool revertTrackProcessing(ProjectModel& model, const TrackId& trackId)
{
    Track* track = model.findTrack(trackId);
    if (track == nullptr)
        return false;
    bool changed = false;
    for (Clip& clip : track->clips) {
        const MediaRef* original = originalMediaOf(model, clip.media);
        if (original != nullptr && original->id != clip.media) {
            clip.media = original->id;
            changed = true;
        }
    }
    if (changed)
        pruneUnusedMedia(model);
    return changed;
}

} // namespace daw
