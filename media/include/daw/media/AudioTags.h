// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Music metadata embedded in exported WAV files. Two standard containers are
// written so that every common player and tagger finds the fields:
//
//  * RIFF "LIST"/"INFO" chunk: read by Windows Explorer, Adobe/Steinberg tools,
//    most DAWs and broadcast software. Strings are UTF-8.
//  * "id3 " chunk carrying an ID3v2.3 tag: read by foobar2000, MusicBee, VLC,
//    Mp3tag, iTunes/Apple Music, Serato and streaming distributors' upload
//    checkers. v2.3 (not v2.4) for the widest compatibility; non-ASCII text is
//    UTF-16 with a BOM.
//
// Dependency-free: no JUCE, no project types.

#include <daw/core/Result.h>

#include <string>
#include <string_view>

namespace daw {

struct AudioTags {
    std::string title;
    std::string artist;
    std::string albumArtist;
    std::string album;
    std::string trackNumber; // "3" or "3/12"
    std::string year;        // "2026"
    std::string genre;
    std::string composer;
    std::string publisher;   // label
    std::string copyright;   // e.g. "© 2026 Example Artist"
    std::string comment;
    std::string isrc;
    std::string url;         // artist website
    std::string software;    // encoder / application
    std::string artwork;         // encoded PNG or JPEG bytes; empty = none
    std::string artworkMimeType; // "image/png" or "image/jpeg"

    bool empty() const noexcept;
    friend bool operator==(const AudioTags&, const AudioTags&) = default;
};

/// Complete "LIST" chunk (header included, even-padded), or empty if no INFO
/// field is set.
std::string encodeRiffInfoChunk(const AudioTags& tags);

/// ID3v2.3 tag bytes (header + frames), or empty if no field is set.
std::string encodeId3v23(const AudioTags& tags);

/// Complete "id3 " RIFF chunk wrapping encodeId3v23, or empty.
std::string encodeId3Chunk(const AudioTags& tags);

/// Reads tags from a WAV file image (LIST/INFO and id3/ID3 chunks; ID3 v2.3
/// and v2.4 text, comment, URL and picture frames). ID3 values win over INFO.
Result<AudioTags> readWavTags(std::string_view wavBytes);

/// "image/png", "image/jpeg" or empty when the bytes are neither.
std::string sniffImageMimeType(std::string_view bytes);

} // namespace daw
