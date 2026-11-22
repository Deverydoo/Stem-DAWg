// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/session/MediaLoader.h>

#include <daw/core/Files.h>
#include <daw/core/Logger.h>
#include <daw/media/PeakCache.h>
#include <daw/media/Resampler.h>
#include <daw/project/ProjectStore.h>

namespace fs = std::filesystem;

namespace daw {

Result<LoadedMedia> retargetMedia(const LoadedMedia& media, double engineSampleRate, const CancellationToken& cancel)
{
    LoadedMedia result = media;
    if (engineSampleRate <= 0.0 || engineSampleRate == media.original->sampleRate) {
        result.rendition = media.original;
        result.renditionRate = media.original->sampleRate;
        return result;
    }
    auto resampled = resampleAudio(*media.original, engineSampleRate, {}, cancel);
    if (!resampled)
        return resampled.error();
    result.rendition = *resampled;
    result.renditionRate = engineSampleRate;
    return result;
}

Result<LoadedMedia> loadMedia(const fs::path& projectRoot, const MediaRef& media, const MediaLoadOptions& options,
                              const CancellationToken& cancel)
{
    const fs::path file = ProjectStore::resolveMediaPath(projectRoot, media);
    auto decoded = decodeAudioFile(file, options.decode, cancel);
    if (!decoded)
        return decoded.error();

    LoadedMedia loaded;
    loaded.id = media.id;
    loaded.original = *decoded;

    // Peaks: reuse the cache when it still matches the media file.
    const fs::path cachePath = ProjectStore::peakCachePath(projectRoot, media.id);
    auto key = computeSourceKey(file);
    if (options.usePeakCache && key) {
        auto cached = readPeakCache(cachePath, *key);
        if (cached) {
            loaded.peaks = std::make_shared<const PeakPyramid>(std::move(*cached));
            loaded.peaksFromCache = true;
        } else if (cached.error().code != ErrorCode::NotFound) {
            logging::info("media", "peak cache rejected; rebuilding",
                          {{"media", media.id.str()}, {"reason", cached.error().describe()}});
        }
    }
    if (!loaded.peaks) {
        auto built = PeakPyramid::build(*loaded.original, {}, cancel);
        if (!built)
            return built.error();
        loaded.peaks = std::make_shared<const PeakPyramid>(std::move(*built));
        if (options.usePeakCache && key) {
            if (Status written = writePeakCache(cachePath, *loaded.peaks, *key); !written)
                logging::warn("media", "peak cache write failed", {{"error", written.error().describe()}});
        }
    }

    return retargetMedia(loaded, options.engineSampleRate, cancel);
}

} // namespace daw
