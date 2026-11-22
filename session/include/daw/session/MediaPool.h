// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Thread-safe registry of decoded media for the open project. Background load
// jobs insert; the control thread reads when building snapshots and drawing.

#include <daw/core/AudioAsset.h>
#include <daw/core/Ids.h>
#include <daw/media/PeakPyramid.h>

#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace daw {

struct LoadedMedia {
    MediaId id;
    AudioAssetPtr original;                    // native sample rate
    AudioAssetPtr rendition;                   // engine sample rate (may equal original)
    double renditionRate = 0.0;
    std::shared_ptr<const PeakPyramid> peaks;  // built from the original
    bool peaksFromCache = false;
};

class MediaPool {
public:
    void put(LoadedMedia media);
    std::optional<LoadedMedia> get(const MediaId& id) const;
    bool contains(const MediaId& id) const;
    void remove(const MediaId& id);
    void clear();
    std::vector<MediaId> ids() const;
    std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<MediaId, LoadedMedia> media_;
};

} // namespace daw
