// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Shared by the rendered (non-destructive) processes, voice denoise and bleed
// suppression: writing a processed copy into Media/Processed, and measuring
// how a process changed the loud and the quiet parts of a recording.

#include <daw/core/AudioAsset.h>
#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>
#include <daw/project/ProjectModel.h>
#include <daw/session/MediaPool.h>

#include <filesystem>
#include <string>
#include <vector>

namespace daw::detail {

inline constexpr const char* kProcessedDir = "Media/Processed";

/// Mean power of each 20 ms block, channels combined.
std::vector<double> blockPowers(const AudioAsset& audio);

/// Level change (dB) from `before` to `after` over the blocks in [low, high)
/// of the original's sounding blocks ranked by level (0.5..1 = the louder
/// half, 0..0.1 = the quietest tenth). Digital silence is left out.
double levelChange(const std::vector<double>& before, const std::vector<double>& after, double low, double high);

struct ProcessedCopy {
    MediaRef media;
    LoadedMedia loaded;
};

/// Writes `audio` as "<original name> (<suffix>).wav" (32-bit float, a free
/// name) into Media/Processed, describes it as derived from `original` by
/// `process` at `amountDb`, and loads it for `engineRate`.
Result<ProcessedCopy> writeProcessedCopy(const std::filesystem::path& projectRoot, const MediaRef& original,
                                         const AudioAsset& audio, const std::string& suffix, const std::string& process,
                                         double amountDb, double engineRate, const CancellationToken& cancel);

} // namespace daw::detail
