// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/session/MediaPool.h>

namespace daw {

void MediaPool::put(LoadedMedia media)
{
    std::lock_guard lock(mutex_);
    const MediaId id = media.id;
    media_[id] = std::move(media);
}

std::optional<LoadedMedia> MediaPool::get(const MediaId& id) const
{
    std::lock_guard lock(mutex_);
    const auto it = media_.find(id);
    if (it == media_.end())
        return std::nullopt;
    return it->second;
}

bool MediaPool::contains(const MediaId& id) const
{
    std::lock_guard lock(mutex_);
    return media_.contains(id);
}

void MediaPool::remove(const MediaId& id)
{
    std::lock_guard lock(mutex_);
    media_.erase(id);
}

void MediaPool::clear()
{
    std::lock_guard lock(mutex_);
    media_.clear();
}

std::vector<MediaId> MediaPool::ids() const
{
    std::lock_guard lock(mutex_);
    std::vector<MediaId> result;
    result.reserve(media_.size());
    for (const auto& [id, media] : media_)
        result.push_back(id);
    return result;
}

std::size_t MediaPool::size() const
{
    std::lock_guard lock(mutex_);
    return media_.size();
}

} // namespace daw
