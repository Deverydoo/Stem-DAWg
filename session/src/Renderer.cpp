// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/session/Renderer.h>

#include <daw/core/Files.h>
#include <daw/engine/AudioEngine.h>
#include <daw/session/MediaLoader.h>
#include <daw/session/SnapshotBuilder.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace daw {

Result<std::unique_ptr<MediaPool>> prepareMediaAtRate(const ProjectModel& model, const MediaPool& pool, double rate,
                                                      const CancellationToken& cancel)
{
    // The original when it already matches, otherwise a fresh high-quality
    // rendition. Only media used by clips.
    auto renderPool = std::make_unique<MediaPool>();
    for (const Track& track : model.tracks)
        for (const Clip& clip : track.clips) {
            if (renderPool->contains(clip.media))
                continue;
            auto loaded = pool.get(clip.media);
            if (!loaded || !loaded->original) {
                const MediaRef* ref = model.findMedia(clip.media);
                return makeError(ErrorCode::NotFound,
                                 "media is not loaded yet: " + (ref != nullptr ? ref->name : clip.media.str()));
            }
            if (loaded->original->sampleRate == rate) {
                loaded->rendition = loaded->original;
                loaded->renditionRate = rate;
            } else if (loaded->renditionRate != rate) {
                auto retargeted = retargetMedia(*loaded, rate, cancel);
                if (!retargeted)
                    return retargeted.error();
                loaded = std::move(*retargeted);
            }
            renderPool->put(std::move(*loaded));
        }
    return renderPool;
}

Result<RenderedMix> renderMix(const ProjectModel& model, const MediaPool& pool, const RenderOptions& options,
                              const CancellationToken& cancel)
{
    const auto started = std::chrono::steady_clock::now();
    const double rate = model.sampleRate;
    if (!(rate > 0.0))
        return makeError(ErrorCode::InvalidArgument, "project has no sample rate");

    auto prepared = prepareMediaAtRate(model, pool, rate, cancel);
    if (!prepared)
        return prepared.error();
    const MediaPool& renderPool = **prepared;

    SnapshotBuildReport buildReport;
    auto snapshot = buildPlaybackSnapshot(model, renderPool, rate, 1, &buildReport);
    if (!buildReport.missingMedia.empty())
        return makeError(ErrorCode::NotFound, "some media could not be prepared for rendering");
    const std::int64_t total = snapshot->endFrame;
    if (total <= 0)
        return makeError(ErrorCode::InvalidArgument, "nothing to render: the project has no audio");

    auto out = AudioAsset::create(2, total, rate);
    const int block = std::max(64, options.blockSize);
    AudioEngine engine;
    engine.prepare({.sampleRate = rate,
                    .maxBlockSize = block,
                    .numOutputChannels = 2,
                    .meterLoudness = false,
                    .maxCleanupTracks = static_cast<int>(model.tracks.size())});
    engine.publishSnapshot(std::move(snapshot));
    engine.seek(0);
    engine.play();

    LoudnessMeter meter;
    meter.prepare(rate, 2);
    RenderReport report;
    report.frames = total;
    report.sampleRate = rate;
    float peak = 0.0f;
    std::int64_t lastProgress = 0;
    for (std::int64_t done = 0; done < total;) {
        if (cancel.isCancelled())
            return makeError(ErrorCode::Cancelled, "render cancelled");
        const int n = static_cast<int>(std::min<std::int64_t>(block, total - done));
        float* outs[2] = {out->channels[0].data() + done, out->channels[1].data() + done};
        engine.process(outs, 2, n);
        meter.process(outs, 2, n);
        for (float* channel : outs)
            for (int i = 0; i < n; ++i) {
                const float a = std::abs(channel[i]);
                peak = std::max(peak, a);
                report.clippedSamples += a > 1.0f ? 1 : 0;
            }
        done += n;
        if (options.progress && (done - lastProgress >= static_cast<std::int64_t>(rate) || done == total)) {
            lastProgress = done;
            options.progress(static_cast<double>(done) / static_cast<double>(total));
        }
    }
    engine.collectGarbage();

    report.loudness = meter.reading();
    report.peakDbfs = peak > 0.0f ? 20.0 * std::log10(static_cast<double>(peak)) : -144.0;
    report.renderSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return RenderedMix{std::move(out), report};
}

AudioTags makeAudioTags(const MetadataFields& m, std::string software, std::string artwork)
{
    AudioTags tags;
    tags.title = m.title;
    tags.artist = m.artist;
    tags.albumArtist = m.albumArtist;
    tags.album = m.album;
    tags.trackNumber = m.trackNumber;
    tags.year = m.year;
    tags.genre = m.genre;
    tags.composer = m.composer;
    tags.publisher = m.publisher;
    tags.copyright = m.copyright;
    tags.comment = m.comment;
    tags.isrc = m.isrc;
    tags.url = m.url;
    tags.software = std::move(software);
    tags.artworkMimeType = sniffImageMimeType(artwork);
    if (!tags.artworkMimeType.empty())
        tags.artwork = std::move(artwork);
    return tags;
}

Result<ExportReport> exportMix(const ProjectModel& model, const MediaPool& pool, const ExportRequest& request,
                               const RenderOptions& options, const CancellationToken& cancel)
{
    ExportReport report;
    std::string artwork;
    if (!request.metadata.artworkPath.empty()) {
        auto bytes = readFileBytes(utf8ToPath(request.metadata.artworkPath));
        if (!bytes)
            report.artworkWarning = "artwork not found: " + request.metadata.artworkPath;
        else if (sniffImageMimeType(*bytes).empty())
            report.artworkWarning = "artwork is not a PNG or JPEG image: " + request.metadata.artworkPath;
        else
            artwork = std::move(*bytes);
    }

    auto mix = renderMix(model, pool, options, cancel);
    if (!mix)
        return mix.error();
    report.render = mix->report;

    const AudioTags tags = makeAudioTags(request.metadata, request.software, std::move(artwork));
    report.artworkEmbedded = !tags.artwork.empty();
    WavEncodeOptions encode;
    encode.format = request.format;
    encode.dither = request.dither && request.format != WavSampleFormat::Float32;
    encode.tags = &tags;
    if (cancel.isCancelled())
        return makeError(ErrorCode::Cancelled, "export cancelled");
    if (auto status = writeWavFile(request.path, *mix->audio, encode); !status)
        return status.error();
    std::error_code ec;
    report.bytes = std::filesystem::file_size(request.path, ec);
    return report;
}

} // namespace daw
