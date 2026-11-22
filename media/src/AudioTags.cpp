// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/media/AudioTags.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace daw {

namespace {

// ---- byte helpers ----------------------------------------------------------

void putU32le(std::string& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void putU32be(std::string& out, std::uint32_t v)
{
    for (int i = 3; i >= 0; --i)
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void putSyncsafe(std::string& out, std::uint32_t v)
{
    for (int i = 3; i >= 0; --i)
        out.push_back(static_cast<char>((v >> (7 * i)) & 0x7F));
}

std::uint32_t getU32le(std::string_view b, std::size_t at)
{
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i)
        v = (v << 8) | static_cast<unsigned char>(b[at + static_cast<std::size_t>(i)]);
    return v;
}

std::uint32_t getU32be(std::string_view b, std::size_t at)
{
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v = (v << 8) | static_cast<unsigned char>(b[at + static_cast<std::size_t>(i)]);
    return v;
}

std::uint32_t getSyncsafe(std::string_view b, std::size_t at)
{
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v = (v << 7) | (static_cast<unsigned char>(b[at + static_cast<std::size_t>(i)]) & 0x7F);
    return v;
}

// ---- text encoding ---------------------------------------------------------

std::vector<char32_t> decodeUtf8(std::string_view s)
{
    std::vector<char32_t> out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        int extra = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
        const bool truncated = extra > 0 && i + static_cast<std::size_t>(extra) >= s.size();
        if (extra < 0 || truncated) {
            out.push_back(U'�');
            ++i;
            continue;
        }
        char32_t cp = extra == 0 ? c : extra == 1 ? (c & 0x1F) : extra == 2 ? (c & 0x0F) : (c & 0x07);
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
            if ((cc & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        out.push_back(ok ? cp : U'�');
        i += ok ? static_cast<std::size_t>(extra) + 1 : 1;
    }
    return out;
}

void appendUtf8(std::string& out, char32_t cp)
{
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool isAscii(std::string_view s)
{
    return std::all_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

/// UTF-16LE with BOM.
std::string toUtf16WithBom(std::string_view utf8)
{
    std::string out("\xFF\xFE", 2);
    for (char32_t cp : decodeUtf8(utf8)) {
        auto put = [&](std::uint16_t u) {
            out.push_back(static_cast<char>(u & 0xFF));
            out.push_back(static_cast<char>(u >> 8));
        };
        if (cp >= 0x10000) {
            cp -= 0x10000;
            put(static_cast<std::uint16_t>(0xD800 + (cp >> 10)));
            put(static_cast<std::uint16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            put(static_cast<std::uint16_t>(cp));
        }
    }
    return out;
}

std::string latin1ToUtf8(std::string_view s)
{
    std::string out;
    for (char c : s)
        appendUtf8(out, static_cast<unsigned char>(c));
    return out;
}

std::string utf16ToUtf8(std::string_view s, bool bigEndian)
{
    std::string out;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        auto unit = [&](std::size_t at) -> std::uint16_t {
            const auto a = static_cast<unsigned char>(s[at]);
            const auto b = static_cast<unsigned char>(s[at + 1]);
            return bigEndian ? static_cast<std::uint16_t>((a << 8) | b) : static_cast<std::uint16_t>((b << 8) | a);
        };
        char32_t cp = unit(i);
        if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < s.size()) {
            const char32_t low = unit(i + 2);
            if (low >= 0xDC00 && low < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        if (cp == 0)
            break;
        appendUtf8(out, cp);
    }
    return out;
}

/// Decodes an ID3 text payload given its encoding byte; stops at the first terminator.
std::string decodeId3Text(unsigned char encoding, std::string_view s)
{
    switch (encoding) {
    case 0:
        return latin1ToUtf8(s.substr(0, s.find('\0')));
    case 1: {
        bool big = false;
        if (s.size() >= 2 && static_cast<unsigned char>(s[0]) == 0xFE && static_cast<unsigned char>(s[1]) == 0xFF) {
            big = true;
            s.remove_prefix(2);
        } else if (s.size() >= 2 && static_cast<unsigned char>(s[0]) == 0xFF && static_cast<unsigned char>(s[1]) == 0xFE) {
            s.remove_prefix(2);
        }
        return utf16ToUtf8(s, big);
    }
    case 2:
        return utf16ToUtf8(s, true);
    default:
        return std::string(s.substr(0, s.find('\0')));
    }
}

/// Offset just past a terminated string in an ID3 payload.
std::size_t skipTerminated(unsigned char encoding, std::string_view s, std::size_t from)
{
    if (encoding == 1 || encoding == 2) {
        for (std::size_t i = from; i + 1 < s.size(); i += 2)
            if (s[i] == '\0' && s[i + 1] == '\0')
                return i + 2;
        return s.size();
    }
    const auto end = s.find('\0', from);
    return end == std::string_view::npos ? s.size() : end + 1;
}

// ---- field tables ----------------------------------------------------------

using Member = std::string AudioTags::*;

struct InfoField {
    const char* id;
    Member member;
};

constexpr InfoField kInfoFields[] = {
    {"INAM", &AudioTags::title},   {"IART", &AudioTags::artist},    {"IPRD", &AudioTags::album},
    {"ITRK", &AudioTags::trackNumber}, {"ICRD", &AudioTags::year},  {"IGNR", &AudioTags::genre},
    {"ICOP", &AudioTags::copyright}, {"ICMT", &AudioTags::comment}, {"ISFT", &AudioTags::software},
};

struct Id3TextField {
    const char* id;
    Member member;
};

constexpr Id3TextField kId3TextFields[] = {
    {"TIT2", &AudioTags::title},     {"TPE1", &AudioTags::artist},   {"TPE2", &AudioTags::albumArtist},
    {"TALB", &AudioTags::album},     {"TRCK", &AudioTags::trackNumber}, {"TYER", &AudioTags::year},
    {"TCON", &AudioTags::genre},     {"TCOM", &AudioTags::composer}, {"TPUB", &AudioTags::publisher},
    {"TCOP", &AudioTags::copyright}, {"TSRC", &AudioTags::isrc},     {"TSSE", &AudioTags::software},
};

/// ID3 TCOP must start with the year; players add the © themselves.
std::string id3Copyright(std::string_view text)
{
    for (std::string_view prefix : {std::string_view("\xC2\xA9"), std::string_view("\xE2\x84\x97"), std::string_view("(C)"),
                                    std::string_view("(c)"), std::string_view("(P)"), std::string_view("(p)")}) {
        if (text.substr(0, prefix.size()) == prefix) {
            text.remove_prefix(prefix.size());
            while (!text.empty() && text.front() == ' ')
                text.remove_prefix(1);
            break;
        }
    }
    return std::string(text);
}

void appendFrame(std::string& tag, const char* id, const std::string& body)
{
    tag.append(id, 4);
    putU32be(tag, static_cast<std::uint32_t>(body.size()));
    tag.append(2, '\0'); // flags
    tag += body;
}

std::string textFrameBody(std::string_view utf8)
{
    if (isAscii(utf8))
        return std::string(1, '\0') + std::string(utf8);
    return std::string(1, '\1') + toUtf16WithBom(utf8);
}

} // namespace

bool AudioTags::empty() const noexcept
{
    for (Member m : {&AudioTags::title, &AudioTags::artist, &AudioTags::albumArtist, &AudioTags::album,
                     &AudioTags::trackNumber, &AudioTags::year, &AudioTags::genre, &AudioTags::composer,
                     &AudioTags::publisher, &AudioTags::copyright, &AudioTags::comment, &AudioTags::isrc,
                     &AudioTags::url, &AudioTags::software, &AudioTags::artwork})
        if (!(this->*m).empty())
            return false;
    return true;
}

std::string sniffImageMimeType(std::string_view b)
{
    if (b.size() >= 8 && b.substr(0, 8) == std::string_view("\x89PNG\r\n\x1A\n", 8))
        return "image/png";
    if (b.size() >= 3 && static_cast<unsigned char>(b[0]) == 0xFF && static_cast<unsigned char>(b[1]) == 0xD8
        && static_cast<unsigned char>(b[2]) == 0xFF)
        return "image/jpeg";
    return {};
}

std::string encodeRiffInfoChunk(const AudioTags& tags)
{
    std::string body = "INFO";
    for (const InfoField& field : kInfoFields) {
        const std::string& value = tags.*field.member;
        if (value.empty())
            continue;
        body.append(field.id, 4);
        putU32le(body, static_cast<std::uint32_t>(value.size() + 1));
        body += value;
        body.push_back('\0');
        if ((value.size() + 1) & 1)
            body.push_back('\0');
    }
    if (body.size() == 4)
        return {};
    std::string chunk = "LIST";
    putU32le(chunk, static_cast<std::uint32_t>(body.size()));
    chunk += body; // always even: every sub-chunk is padded
    return chunk;
}

std::string encodeId3v23(const AudioTags& tags)
{
    std::string frames;
    for (const Id3TextField& field : kId3TextFields) {
        std::string value = tags.*field.member;
        if (field.member == &AudioTags::copyright)
            value = id3Copyright(value);
        if (!value.empty())
            appendFrame(frames, field.id, textFrameBody(value));
    }
    if (!tags.comment.empty()) {
        // encoding, language, empty short description, text
        std::string body;
        if (isAscii(tags.comment)) {
            body = std::string(1, '\0') + "eng" + std::string(1, '\0') + tags.comment;
        } else {
            body = std::string(1, '\1') + "eng" + std::string("\xFF\xFE\0\0", 4) + toUtf16WithBom(tags.comment);
        }
        appendFrame(frames, "COMM", body);
    }
    if (!tags.url.empty())
        appendFrame(frames, "WOAR", tags.url); // URL frames are always ISO-8859-1
    if (!tags.artwork.empty()) {
        const std::string mime = tags.artworkMimeType.empty() ? sniffImageMimeType(tags.artwork) : tags.artworkMimeType;
        std::string body(1, '\0');           // description encoding
        body += mime.empty() ? "image/" : mime;
        body.push_back('\0');
        body.push_back('\x03');              // front cover
        body.push_back('\0');                // empty description
        body += tags.artwork;
        appendFrame(frames, "APIC", body);
    }
    if (frames.empty())
        return {};

    std::string tag = "ID3";
    tag.push_back('\x03'); // v2.3
    tag.push_back('\0');   // revision
    tag.push_back('\0');   // flags
    putSyncsafe(tag, static_cast<std::uint32_t>(frames.size()));
    tag += frames;
    return tag;
}

std::string encodeId3Chunk(const AudioTags& tags)
{
    const std::string tag = encodeId3v23(tags);
    if (tag.empty())
        return {};
    std::string chunk = "id3 ";
    putU32le(chunk, static_cast<std::uint32_t>(tag.size()));
    chunk += tag;
    if (tag.size() & 1)
        chunk.push_back('\0');
    return chunk;
}

namespace {

void readInfo(std::string_view list, AudioTags& tags)
{
    for (std::size_t pos = 4; pos + 8 <= list.size();) {
        const std::string_view id = list.substr(pos, 4);
        const std::uint32_t size = getU32le(list, pos + 4);
        if (pos + 8 + size > list.size())
            break;
        std::string_view value = list.substr(pos + 8, size);
        value = value.substr(0, value.find('\0'));
        for (const InfoField& field : kInfoFields)
            if (id == field.id)
                tags.*field.member = std::string(value);
        pos += 8 + size + (size & 1);
    }
}

void readId3(std::string_view tag, AudioTags& tags)
{
    if (tag.size() < 10 || tag.substr(0, 3) != "ID3")
        return;
    const int version = static_cast<unsigned char>(tag[3]);
    if (version != 3 && version != 4)
        return;
    const std::size_t end = std::min<std::size_t>(tag.size(), 10 + getSyncsafe(tag, 6));
    for (std::size_t pos = 10; pos + 10 <= end;) {
        const std::string_view id = tag.substr(pos, 4);
        if (id[0] == '\0')
            break; // padding
        const std::uint32_t size = version == 4 ? getSyncsafe(tag, pos + 4) : getU32be(tag, pos + 4);
        if (pos + 10 + size > end)
            break;
        const std::string_view body = tag.substr(pos + 10, size);
        pos += 10 + size;
        if (body.empty())
            continue;
        const auto encoding = static_cast<unsigned char>(body[0]);

        bool matched = false;
        for (const Id3TextField& field : kId3TextFields)
            if (id == field.id || (field.member == &AudioTags::year && id == "TDRC")) {
                tags.*field.member = decodeId3Text(encoding, body.substr(1));
                matched = true;
                break;
            }
        if (matched)
            continue;
        if (id == "COMM" && body.size() > 4) {
            const std::size_t text = skipTerminated(encoding, body, 4);
            tags.comment = decodeId3Text(encoding, body.substr(text));
        } else if (id == "WOAR") {
            tags.url = latin1ToUtf8(body.substr(0, body.find('\0')));
        } else if (id == "APIC" && body.size() > 4) {
            const std::size_t mimeEnd = body.find('\0', 1);
            if (mimeEnd == std::string_view::npos || mimeEnd + 2 >= body.size())
                continue;
            tags.artworkMimeType = std::string(body.substr(1, mimeEnd - 1));
            const std::size_t data = skipTerminated(encoding, body, mimeEnd + 2);
            tags.artwork = std::string(body.substr(data));
        }
    }
}

} // namespace

Result<AudioTags> readWavTags(std::string_view b)
{
    if (b.size() < 12 || b.substr(0, 4) != "RIFF" || b.substr(8, 4) != "WAVE")
        return makeError(ErrorCode::Corrupt, "not a RIFF/WAVE file");
    AudioTags info;
    AudioTags id3;
    bool haveId3 = false;
    for (std::size_t pos = 12; pos + 8 <= b.size();) {
        const std::string_view id = b.substr(pos, 4);
        const std::uint32_t size = getU32le(b, pos + 4);
        const std::size_t available = std::min<std::size_t>(size, b.size() - pos - 8);
        const std::string_view body = b.substr(pos + 8, available);
        if (id == "LIST" && body.substr(0, 4) == "INFO")
            readInfo(body, info);
        else if (id == "id3 " || id == "ID3 ") {
            readId3(body, id3);
            haveId3 = true;
        }
        pos += 8 + static_cast<std::size_t>(size) + (size & 1);
    }
    if (!haveId3)
        return info;
    // ID3 wins; INFO fills anything ID3 lacks.
    for (Member m : {&AudioTags::title, &AudioTags::artist, &AudioTags::album, &AudioTags::trackNumber, &AudioTags::year,
                     &AudioTags::genre, &AudioTags::copyright, &AudioTags::comment, &AudioTags::software})
        if ((id3.*m).empty())
            id3.*m = info.*m;
    // TCOP drops the leading (C) symbol; keep INFO's fuller form when it matches.
    if (info.copyright.size() > id3.copyright.size()
        && info.copyright.compare(info.copyright.size() - id3.copyright.size(), std::string::npos, id3.copyright) == 0)
        id3.copyright = info.copyright;
    return id3;
}

} // namespace daw
