// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/media/TarGzArchive.h>

#include "TarFormat.h"

#include <daw/core/Files.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>

namespace fs = std::filesystem;

namespace daw {

Status extractTarGzFiles(const fs::path& archive, const std::vector<std::string>& fileNames, const fs::path& outputDir,
                         std::size_t maxBytes, const CancellationToken& cancel)
{
    std::error_code ec;
    if (!fs::is_regular_file(archive, ec))
        return makeError(ErrorCode::NotFound, "cannot open " + pathToUtf8(archive));

    // Decompress into memory.
    auto file = std::make_unique<juce::FileInputStream>(juce::File(juce::String(archive.wstring().c_str())));
    if (!file->openedOk())
        return makeError(ErrorCode::IoError, "cannot open " + pathToUtf8(archive));
    juce::GZIPDecompressorInputStream gzip(file.release(), true, juce::GZIPDecompressorInputStream::gzipFormat);
    std::vector<char> tar;
    std::vector<char> chunk(1 << 16);
    for (;;) {
        if (cancel.isCancelled())
            return makeError(ErrorCode::Cancelled, "extraction cancelled");
        const int n = gzip.read(chunk.data(), static_cast<int>(chunk.size()));
        if (n <= 0)
            break;
        if (tar.size() + static_cast<std::size_t>(n) > maxBytes)
            return makeError(ErrorCode::LimitExceeded, "the archive is larger than expected: " + pathToUtf8(archive));
        tar.insert(tar.end(), chunk.data(), chunk.data() + n);
    }
    if (tar.size() < 512)
        return makeError(ErrorCode::Corrupt, "not a .tar.gz archive, or it is damaged: " + pathToUtf8(archive));

    // Walk the tar entries.
    std::map<std::string, std::pair<std::size_t, std::size_t>> found; // name -> (offset, size)
    std::string longName;
    for (std::size_t at = 0; at + 512 <= tar.size();) {
        const char* header = tar.data() + at;
        if (std::all_of(header, header + 512, [](char c) { return c == '\0'; }))
            break; // end-of-archive marker
        std::string name = !longName.empty() ? longName : archive::trimField(header, 100);
        if (longName.empty() && std::memcmp(header + 257, "ustar", 5) == 0) {
            const std::string prefix = archive::trimField(header + 345, 155);
            if (!prefix.empty())
                name = prefix + "/" + name;
        }
        longName.clear();
        const std::uint64_t size = archive::parseOctal(header + 124, 12);
        const std::size_t data = at + 512;
        if (size > tar.size() - data)
            return makeError(ErrorCode::Corrupt, "the archive is truncated: " + pathToUtf8(archive));
        const char type = header[156];
        if (type == 'L') {
            longName = archive::trimField(tar.data() + data, static_cast<std::size_t>(size));
        } else if (type == '0' || type == '\0') {
            const std::string base = name.substr(name.find_last_of('/') + 1);
            if (std::find(fileNames.begin(), fileNames.end(), base) != fileNames.end())
                found[base] = {data, static_cast<std::size_t>(size)};
        }
        at = data + static_cast<std::size_t>((size + 511) / 512 * 512);
    }
    for (const std::string& name : fileNames)
        if (!found.contains(name))
            return makeError(ErrorCode::NotFound, "the archive does not contain " + name);

    // Write each file atomically; remove everything written on failure.
    fs::create_directories(outputDir, ec);
    if (ec)
        return makeError(ErrorCode::IoError, "cannot create " + pathToUtf8(outputDir) + ": " + ec.message());
    std::vector<fs::path> partials;
    const auto cleanUp = [&] {
        for (const fs::path& p : partials)
            fs::remove(p, ec);
    };
    for (const std::string& name : fileNames) {
        const auto [offset, size] = found[name];
        const fs::path partial = outputDir / utf8ToPath(name + ".part");
        partials.push_back(partial);
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out.write(tar.data() + offset, static_cast<std::streamsize>(size));
        out.close();
        if (!out) {
            cleanUp();
            return makeError(ErrorCode::IoError, "cannot write " + pathToUtf8(partial));
        }
    }
    for (const std::string& name : fileNames) {
        const fs::path target = outputDir / utf8ToPath(name);
        fs::remove(target, ec);
        fs::rename(outputDir / utf8ToPath(name + ".part"), target, ec);
        if (ec) {
            cleanUp();
            return makeError(ErrorCode::IoError, "cannot move " + name + " into place: " + ec.message());
        }
    }
    return Status::success();
}

} // namespace daw
