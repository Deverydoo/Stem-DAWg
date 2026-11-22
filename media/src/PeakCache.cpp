// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/media/PeakCache.h>

#include <daw/core/Files.h>
#include <daw/core/Hash.h>

#include <cstring>
#include <type_traits>

namespace fs = std::filesystem;

namespace daw {

namespace {

constexpr char kMagic[4] = {'I', 'D', 'P', 'K'};

class Writer {
public:
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void put(const T& value)
    {
        const auto* p = reinterpret_cast<const char*>(&value);
        bytes.append(p, sizeof(T)); // x64 Windows is little-endian
    }
    void putRaw(const void* data, std::size_t size) { bytes.append(static_cast<const char*>(data), size); }
    std::string bytes;
};

class Reader {
public:
    explicit Reader(std::string_view data) : data_(data) {}

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    bool get(T& value)
    {
        if (data_.size() - pos_ < sizeof(T))
            return false;
        std::memcpy(&value, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return true;
    }

    bool getRaw(void* out, std::size_t size)
    {
        if (data_.size() - pos_ < size)
            return false;
        std::memcpy(out, data_.data() + pos_, size);
        pos_ += size;
        return true;
    }

    std::size_t remaining() const { return data_.size() - pos_; }

private:
    std::string_view data_;
    std::size_t pos_ = 0;
};

} // namespace

Result<std::uint64_t> computeSourceKey(const fs::path& mediaFile)
{
    std::error_code ec;
    const auto size = fs::file_size(mediaFile, ec);
    if (ec)
        return makeError(ErrorCode::NotFound, "cannot stat media file: " + pathToUtf8(mediaFile));
    const auto written = fs::last_write_time(mediaFile, ec);
    if (ec)
        return makeError(ErrorCode::IoError, "cannot read modification time: " + pathToUtf8(mediaFile));

    Fnv1a64 h;
    h.update(pathToUtf8(mediaFile.filename()));
    h.updateValue(static_cast<std::uint64_t>(size));
    h.updateValue(static_cast<std::int64_t>(written.time_since_epoch().count()));
    return h.digest();
}

std::string encodePeakCache(const PeakPyramid& pyramid, std::uint64_t sourceKey)
{
    Writer w;
    w.putRaw(kMagic, sizeof(kMagic));
    w.put(kPeakCacheVersion);
    w.put(sourceKey);
    w.put(pyramid.sampleRate());
    w.put(static_cast<std::uint32_t>(pyramid.numChannels()));
    w.put(pyramid.numFrames());
    w.put(pyramid.params().baseSamplesPerBucket);
    w.put(static_cast<std::uint32_t>(pyramid.params().factor));
    w.put(pyramid.params().minBuckets);
    w.put(static_cast<std::uint32_t>(pyramid.levels().size()));
    for (const PeakLevel& level : pyramid.levels()) {
        w.put(level.samplesPerBucket);
        w.put(level.numBuckets);
    }
    for (const PeakLevel& level : pyramid.levels())
        w.putRaw(level.data.data(), level.data.size() * sizeof(PeakBucket));

    Fnv1a64 checksum;
    checksum.update(w.bytes.data(), w.bytes.size());
    w.put(checksum.digest());
    return std::move(w.bytes);
}

Result<PeakPyramid> decodePeakCache(std::string_view bytes, std::uint64_t expectedSourceKey)
{
    if (bytes.size() < sizeof(kMagic) + sizeof(std::uint64_t))
        return makeError(ErrorCode::Corrupt, "peak cache truncated");

    const std::string_view body = bytes.substr(0, bytes.size() - sizeof(std::uint64_t));
    std::uint64_t storedChecksum = 0;
    std::memcpy(&storedChecksum, bytes.data() + body.size(), sizeof(storedChecksum));
    Fnv1a64 checksum;
    checksum.update(body.data(), body.size());
    if (checksum.digest() != storedChecksum)
        return makeError(ErrorCode::Corrupt, "peak cache checksum mismatch");

    Reader r(body);
    char magic[4] = {};
    std::uint32_t version = 0;
    std::uint64_t sourceKey = 0;
    double sampleRate = 0.0;
    std::uint32_t channels = 0;
    std::int64_t frames = 0;
    PeakPyramidParams params;
    std::uint32_t factor = 0;
    std::uint32_t levelCount = 0;

    if (!r.getRaw(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
        return makeError(ErrorCode::Corrupt, "not a peak cache file");
    if (!r.get(version) || version != kPeakCacheVersion)
        return makeError(ErrorCode::Stale, "unsupported peak cache version");
    if (!r.get(sourceKey) || !r.get(sampleRate) || !r.get(channels) || !r.get(frames)
        || !r.get(params.baseSamplesPerBucket) || !r.get(factor) || !r.get(params.minBuckets) || !r.get(levelCount))
        return makeError(ErrorCode::Corrupt, "peak cache header truncated");
    if (sourceKey != expectedSourceKey)
        return makeError(ErrorCode::Stale, "peak cache does not match the media file");
    if (channels == 0 || channels > 64 || levelCount == 0 || levelCount > 64 || factor < 2)
        return makeError(ErrorCode::Corrupt, "peak cache header out of range");
    params.factor = static_cast<int>(factor);

    std::vector<PeakLevel> levels(levelCount);
    for (PeakLevel& level : levels) {
        if (!r.get(level.samplesPerBucket) || !r.get(level.numBuckets) || level.numBuckets < 0)
            return makeError(ErrorCode::Corrupt, "peak cache level table truncated");
    }
    for (PeakLevel& level : levels) {
        const auto count = static_cast<std::size_t>(level.numBuckets) * channels;
        if (r.remaining() < count * sizeof(PeakBucket))
            return makeError(ErrorCode::Corrupt, "peak cache data truncated");
        level.data.resize(count);
        r.getRaw(level.data.data(), count * sizeof(PeakBucket));
    }
    if (r.remaining() != 0)
        return makeError(ErrorCode::Corrupt, "peak cache has trailing data");

    return PeakPyramid::fromLevels(static_cast<int>(channels), frames, sampleRate, params, std::move(levels));
}

Status writePeakCache(const fs::path& file, const PeakPyramid& pyramid, std::uint64_t sourceKey)
{
    return writeFileAtomically(file, encodePeakCache(pyramid, sourceKey));
}

Result<PeakPyramid> readPeakCache(const fs::path& file, std::uint64_t expectedSourceKey)
{
    auto bytes = readFileBytes(file);
    if (!bytes)
        return bytes.error();
    return decodePeakCache(*bytes, expectedSourceKey);
}

} // namespace daw
