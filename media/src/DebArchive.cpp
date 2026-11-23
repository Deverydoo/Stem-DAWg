// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/media/DebArchive.h>

#include "TarFormat.h"

#include <daw/core/Files.h>

extern "C" {
#include <xz.h>
}

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace daw {

namespace {

using archive::parseOctal;
using archive::trimField;

bool endsWithPath(const std::string& path, std::string_view suffix)
{
    std::string_view p = path;
    while (p.starts_with("./"))
        p.remove_prefix(2);
    if (p.size() < suffix.size() || p.substr(p.size() - suffix.size()) != suffix)
        return false;
    return p.size() == suffix.size() || p[p.size() - suffix.size() - 1] == '/';
}

/// Incremental tar reader: finds one regular file and writes it out.
class TarExtractor {
public:
    TarExtractor(std::string_view member, std::ofstream& out) : member_(member), out_(out) {}

    /// Consumes decompressed bytes. Returns false once the member is complete
    /// or the archive has ended (no more input needed).
    bool feed(const std::uint8_t* data, std::size_t size)
    {
        while (size > 0 && !finished_) {
            if (remaining_ == 0) {
                const std::size_t take = std::min(size, header_.size() - headerFill_);
                std::memcpy(header_.data() + headerFill_, data, take);
                headerFill_ += take;
                data += take;
                size -= take;
                if (headerFill_ == header_.size()) {
                    headerFill_ = 0;
                    parseHeader();
                }
                continue;
            }
            const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(size, remaining_));
            consumeData(data, take);
            remaining_ -= take;
            data += take;
            size -= take;
            if (remaining_ == 0)
                endEntry();
        }
        return !finished_;
    }

    bool found() const noexcept { return found_; }
    bool writeFailed() const noexcept { return writeFailed_; }

private:
    enum class Mode { Skip, Write, LongName };

    void parseHeader()
    {
        if (std::all_of(header_.begin(), header_.end(), [](char c) { return c == '\0'; })) {
            finished_ = true; // end-of-archive marker
            return;
        }
        std::string name = !longName_.empty() ? longName_ : trimField(header_.data(), 100);
        if (longName_.empty() && std::memcmp(header_.data() + 257, "ustar", 5) == 0) {
            const std::string prefix = trimField(header_.data() + 345, 155);
            if (!prefix.empty())
                name = prefix + "/" + name;
        }
        longName_.clear();
        size_ = parseOctal(header_.data() + 124, 12);
        remaining_ = (size_ + 511) / 512 * 512;
        dataLeft_ = size_;
        const char type = header_[156];
        if (type == 'L') {
            mode_ = Mode::LongName;
        } else if ((type == '0' || type == '\0') && endsWithPath(name, member_)) {
            mode_ = Mode::Write;
        } else {
            mode_ = Mode::Skip;
        }
        if (remaining_ == 0)
            endEntry();
    }

    void consumeData(const std::uint8_t* data, std::size_t size)
    {
        const std::size_t useful = static_cast<std::size_t>(std::min<std::uint64_t>(size, dataLeft_));
        dataLeft_ -= useful;
        if (mode_ == Mode::Write) {
            out_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(useful));
            writeFailed_ = writeFailed_ || !out_;
        } else if (mode_ == Mode::LongName) {
            pendingName_.append(reinterpret_cast<const char*>(data), useful);
        }
    }

    void endEntry()
    {
        if (mode_ == Mode::Write) {
            found_ = true;
            finished_ = true;
        } else if (mode_ == Mode::LongName) {
            longName_ = trimField(pendingName_.data(), pendingName_.size());
            pendingName_.clear();
        }
        mode_ = Mode::Skip;
    }

    std::string_view member_;
    std::ofstream& out_;
    std::array<char, 512> header_{};
    std::size_t headerFill_ = 0;
    std::uint64_t size_ = 0, remaining_ = 0, dataLeft_ = 0;
    Mode mode_ = Mode::Skip;
    std::string longName_, pendingName_;
    bool found_ = false, finished_ = false, writeFailed_ = false;
};

struct XzDecoder {
    xz_dec* state = nullptr;
    ~XzDecoder()
    {
        if (state != nullptr)
            xz_dec_end(state);
    }
};

} // namespace

Status extractDebMember(const fs::path& deb, std::string_view memberPath, const fs::path& output,
                        const CancellationToken& cancel, const std::function<void(double)>& progress)
{
    std::ifstream in(deb, std::ios::binary);
    if (!in)
        return makeError(ErrorCode::NotFound, "cannot open " + pathToUtf8(deb));
    std::error_code ec;
    const auto packageSize = fs::file_size(deb, ec);

    char magic[8];
    if (!in.read(magic, 8) || std::memcmp(magic, "!<arch>\n", 8) != 0)
        return makeError(ErrorCode::UnsupportedFormat, "not a Debian package: " + pathToUtf8(deb));

    // Find data.tar.xz among the ar members.
    std::uint64_t dataSize = 0;
    for (;;) {
        char header[60];
        if (!in.read(header, 60))
            return makeError(ErrorCode::Corrupt, "the package has no data.tar.xz: " + pathToUtf8(deb));
        const std::string name = trimField(header, 16);
        const std::uint64_t size = std::strtoull(std::string(header + 48, 10).c_str(), nullptr, 10);
        if (name == "data.tar.xz") {
            dataSize = size;
            break;
        }
        if (name.starts_with("data.tar"))
            return makeError(ErrorCode::UnsupportedFormat, "unsupported package compression: " + name);
        in.seekg(static_cast<std::streamoff>(size + (size & 1)), std::ios::cur);
    }

    const fs::path partial = output.parent_path() / (output.filename().string() + ".part");
    fs::create_directories(output.parent_path(), ec);
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    if (!out)
        return makeError(ErrorCode::IoError, "cannot write " + pathToUtf8(partial));
    TarExtractor tar(memberPath, out);

    xz_crc32_init();
    xz_crc64_init();
    XzDecoder decoder;
    decoder.state = xz_dec_init(XZ_DYNALLOC, 1u << 27);
    if (decoder.state == nullptr)
        return makeError(ErrorCode::Unknown, "out of memory");

    std::vector<std::uint8_t> input(1 << 16), decoded(1 << 20);
    xz_buf buffer{};
    buffer.in = input.data();
    buffer.out = decoded.data();
    buffer.out_size = decoded.size();
    std::uint64_t left = dataSize, consumed = 0, lastProgress = 0;
    bool more = true;
    Status result = Status::success();

    while (more) {
        if (cancel.isCancelled()) {
            result = makeError(ErrorCode::Cancelled, "extraction cancelled");
            break;
        }
        if (buffer.in_pos == buffer.in_size && left > 0) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), left));
            if (!in.read(reinterpret_cast<char*>(input.data()), static_cast<std::streamsize>(n))) {
                result = makeError(ErrorCode::Corrupt, "the package is truncated");
                break;
            }
            left -= n;
            consumed += n;
            buffer.in_pos = 0;
            buffer.in_size = n;
            if (progress && packageSize > 0 && consumed - lastProgress >= (8u << 20)) {
                lastProgress = consumed;
                progress(static_cast<double>(consumed) / static_cast<double>(packageSize));
            }
        }
        const xz_ret ret = xz_dec_run(decoder.state, &buffer);
        more = tar.feed(decoded.data(), buffer.out_pos);
        buffer.out_pos = 0;
        if (tar.writeFailed()) {
            result = makeError(ErrorCode::IoError, "write failed: " + pathToUtf8(partial));
            break;
        }
        if (ret == XZ_STREAM_END)
            break;
        if (ret != XZ_OK && ret != XZ_UNSUPPORTED_CHECK) {
            result = makeError(ErrorCode::Corrupt, "the package data is damaged (xz error " + std::to_string(static_cast<int>(ret)) + ")");
            break;
        }
    }
    out.close();

    if (result && !tar.found())
        result = makeError(ErrorCode::NotFound, std::string("the package does not contain ") + std::string(memberPath));
    if (!result) {
        fs::remove(partial, ec);
        return result;
    }
    fs::remove(output, ec);
    fs::rename(partial, output, ec);
    if (ec)
        return makeError(ErrorCode::IoError, "cannot move " + pathToUtf8(partial) + ": " + ec.message());
    if (progress)
        progress(1.0);
    return Status::success();
}

} // namespace daw
