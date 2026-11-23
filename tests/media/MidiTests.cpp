// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// MIDI probing and SoundFont rendering, the .deb extractor used to install the
// SoundFont, and SHA-256. The MIDI file and the SoundFont are built here in
// code: a one-note song and a SoundFont holding a single looped 480 Hz sine.

#include "TempDir.h"
#include "TestMidi.h"
#include "TestDeb.h"
#include "TestSignals.h"

#include <daw/core/Files.h>
#include <daw/core/Sha256.h>
#include <daw/media/DebArchive.h>
#include <daw/media/Midi.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

using namespace daw;
using namespace daw::test;
using Catch::Approx;

namespace {

double rms(const std::vector<float>& x, std::int64_t begin, std::int64_t end)
{
    double sum = 0.0;
    for (std::int64_t i = begin; i < end; ++i)
        sum += static_cast<double>(x[static_cast<std::size_t>(i)]) * x[static_cast<std::size_t>(i)];
    return std::sqrt(sum / static_cast<double>(end - begin));
}

} // namespace

TEST_CASE("MIDI files are recognised by extension", "[media][midi]")
{
    CHECK(isMidiFile("song.mid"));
    CHECK(isMidiFile("C:/x/Song (Drums).MIDI"));
    CHECK_FALSE(isMidiFile("song.wav"));
    CHECK_FALSE(isMidiFile("mid"));
}

TEST_CASE("Probing a MIDI file reads length, tempo and notes", "[media][midi]")
{
    TempDir dir("midi");
    REQUIRE(writeFileAtomically(dir / "one.mid", oneNoteSong()).ok());
    const auto info = probeMidiFile(dir / "one.mid");
    REQUIRE(info.ok());
    CHECK(info->durationSeconds == Approx(1.5).margin(0.001));
    CHECK(info->averageBpm == Approx(120.0).margin(0.01));
    CHECK(info->noteCount == 1);
    CHECK(info->channelCount == 1);
    CHECK_FALSE(info->usesDrums);
    CHECK(midiRenderFrames(*info, kMidiTestRate) == static_cast<std::int64_t>((1.5 + kMidiTailSeconds) * kMidiTestRate));

    // Two quarters at 100 BPM (1.2 s), then two at 150 BPM (0.8 s): 4 quarters in 2 s = 120 BPM on average.
    REQUIRE(writeFileAtomically(dir / "tempo.mid",
                                makeMidi({{0, tempo(600000)}, {0, noteOn(9, 36, 90)}, {960, tempo(400000)}, {1920, noteOff(9, 36)}}))
                .ok());
    const auto changing = probeMidiFile(dir / "tempo.mid");
    REQUIRE(changing.ok());
    CHECK(changing->durationSeconds == Approx(2.0).margin(0.002));
    CHECK(changing->averageBpm == Approx(120.0).margin(0.2));
    CHECK(changing->usesDrums);

    REQUIRE(writeFileAtomically(dir / "empty.mid", makeMidi({{0, tempo(500000)}})).ok());
    CHECK_FALSE(probeMidiFile(dir / "empty.mid").ok());
    REQUIRE(writeFileAtomically(dir / "junk.mid", std::string("not midi at all")).ok());
    CHECK_FALSE(probeMidiFile(dir / "junk.mid").ok());
    CHECK_FALSE(probeMidiFile(dir / "missing.mid").ok());
}

TEST_CASE("A MIDI file renders through a SoundFont at the right time and pitch", "[media][midi]")
{
    TempDir dir("midi");
    // Unicode file names must work (the loaders' own file APIs cannot open them on Windows).
    const auto font = dir / utf8ToPath("Sinus \xc3\xa9t\xc3\xa9.sf2");
    const auto song = dir / utf8ToPath("Ch\xc3\xb6r.mid");
    REQUIRE(writeFileAtomically(font, makeSoundFont()).ok());
    REQUIRE(writeFileAtomically(song, oneNoteSong()).ok());

    const auto loaded = loadSoundFont(font);
    REQUIRE(loaded.ok());
    CHECK((*loaded)->presetCount() == 1);
    const auto again = loadSoundFont(font);
    REQUIRE(again.ok());
    CHECK(again->get() == loaded->get()); // shared, loaded once

    double lastProgress = 0.0;
    const auto audio = renderMidiFile(song, **loaded, {kMidiTestRate, 0.0f}, {}, [&](double p) { lastProgress = p; });
    REQUIRE(audio.ok());
    const AudioAsset& a = **audio;
    CHECK(lastProgress == 1.0);
    REQUIRE(a.numChannels() == 2);
    CHECK(a.sampleRate == kMidiTestRate);
    CHECK(a.numFrames() == static_cast<std::int64_t>(3.5 * kMidiTestRate));

    const auto& left = a.channels[0];
    CHECK(rms(left, 0, 47500) < 1e-6);                     // nothing before the note
    CHECK(rms(left, 48500, 71500) > 0.05);                 // the note
    CHECK(rms(left, static_cast<std::int64_t>(1.7 * kMidiTestRate), a.numFrames()) < 1e-3); // released

    // Onset within a millisecond of 1.0 s.
    std::int64_t onset = 0;
    while (onset < a.numFrames() && std::abs(left[static_cast<std::size_t>(onset)]) < 1e-4f)
        ++onset;
    CHECK(std::abs(onset - 48000) <= 60);

    // Key 71 is the sample's root key: it plays at 480 Hz.
    const double atPitch = goertzelPower(left.data() + 50000, 19200, 480.0, kMidiTestRate);
    const double elsewhere = goertzelPower(left.data() + 50000, 19200, 700.0, kMidiTestRate);
    CHECK(atPitch > 100.0 * elsewhere);

    CHECK_FALSE(loadSoundFont(dir / "missing.sf2").ok());
    REQUIRE(writeFileAtomically(dir / "bad.sf2", std::string("RIFF\x04\x00\x00\x00junk", 12)).ok());
    CHECK_FALSE(loadSoundFont(dir / "bad.sf2").ok());

    CancellationSource cancel;
    cancel.cancel();
    const auto cancelled = renderMidiFile(song, **loaded, {kMidiTestRate, 0.0f}, cancel.token());
    REQUIRE_FALSE(cancelled.ok());
    CHECK(cancelled.error().code == ErrorCode::Cancelled);
}

TEST_CASE("A file is extracted from a Debian package without external tools", "[media][deb]")
{
    TempDir dir("deb");
    const auto deb = dir / "test.deb";
    REQUIRE(writeFileAtomically(deb, std::string(reinterpret_cast<const char*>(kTestDeb), sizeof kTestDeb)).ok());

    std::string expected;
    for (int i = 0; i < 40; ++i)
        expected += "Stem DAWg test SoundFont payload\n";

    double lastProgress = 0.0;
    const auto out = dir / "fonts" / "Test.sf2";
    REQUIRE(extractDebMember(deb, "usr/share/sounds/sf2/Test.sf2", out, {}, [&](double p) { lastProgress = p; }).ok());
    CHECK(readFileBytes(out).value() == expected);
    CHECK(lastProgress == 1.0);
    CHECK_FALSE(std::filesystem::exists(dir / "fonts" / "Test.sf2.part"));

    // A member behind a GNU long path name is found by its suffix too.
    REQUIRE(extractDebMember(deb, "Other.sf2", dir / "other.sf2").ok());
    CHECK(readFileBytes(dir / "other.sf2").value() == "not this one\n");

    // Only whole path components match.
    const auto partial = extractDebMember(deb, "est.sf2", dir / "x.sf2");
    REQUIRE_FALSE(partial.ok());
    CHECK(partial.error().code == ErrorCode::NotFound);
    CHECK_FALSE(std::filesystem::exists(dir / "x.sf2"));

    // Damaged data is rejected and leaves nothing behind.
    std::string damaged(reinterpret_cast<const char*>(kTestDeb), sizeof kTestDeb);
    damaged[damaged.size() - 40] ^= 0x5a;
    REQUIRE(writeFileAtomically(dir / "damaged.deb", damaged).ok());
    CHECK_FALSE(extractDebMember(dir / "damaged.deb", "usr/share/sounds/sf2/Test.sf2", dir / "d.sf2").ok());
    CHECK_FALSE(std::filesystem::exists(dir / "d.sf2"));

    REQUIRE(writeFileAtomically(dir / "plain.txt", std::string("hello")).ok());
    CHECK_FALSE(extractDebMember(dir / "plain.txt", "Test.sf2", dir / "p.sf2").ok());
}

TEST_CASE("SHA-256 matches the FIPS 180-4 test vectors", "[core][sha256]")
{
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
          == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // Streaming in odd pieces gives the same digest as one call.
    const std::string million(1000000, 'a');
    Sha256 streamed;
    for (std::size_t at = 0; at < million.size(); at += 997)
        streamed.update(million.data() + at, std::min<std::size_t>(997, million.size() - at));
    CHECK(streamed.finishHex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    TempDir dir("sha");
    REQUIRE(writeFileAtomically(dir / "a.txt", million).ok());
    CHECK(sha256File(dir / "a.txt").value() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    CHECK_FALSE(sha256File(dir / "missing").ok());
}

// Opt-in check against the real FluidR3_GM package and a real MIDI file:
//   set STEMDAWG_TEST_DEB=<path to fluid-soundfont-gm_3.1-5.3_all.deb>
//   set STEMDAWG_TEST_MIDI=<path to a .mid file>
//   daw_media_tests "[real-soundfont]"
TEST_CASE("Real FluidR3_GM package installs and renders a song", "[.][real-soundfont]")
{
    const char* debPath = std::getenv("STEMDAWG_TEST_DEB");
    const char* midiPath = std::getenv("STEMDAWG_TEST_MIDI");
    if (debPath == nullptr || midiPath == nullptr)
        SKIP("STEMDAWG_TEST_DEB / STEMDAWG_TEST_MIDI not set");

    CHECK(sha256File(utf8ToPath(debPath)).value() == kFluidR3Gm.packageSha256);
    TempDir dir("fluid");
    const auto font = dir / kFluidR3Gm.fileName;
    REQUIRE(extractDebMember(utf8ToPath(debPath), kFluidR3Gm.member, font).ok());
    CHECK(std::filesystem::file_size(font) == kFluidR3Gm.fileBytes);
    CHECK(sha256File(font).value() == kFluidR3Gm.fileSha256);

    const auto loaded = loadSoundFont(font);
    REQUIRE(loaded.ok());
    CHECK((*loaded)->presetCount() > 128);
    const auto info = probeMidiFile(utf8ToPath(midiPath));
    REQUIRE(info.ok());
    const auto audio = renderMidiFile(utf8ToPath(midiPath), **loaded, {48000.0, 0.0f});
    REQUIRE(audio.ok());
    float peak = 0.0f;
    for (const auto& channel : (*audio)->channels)
        for (float s : channel)
            peak = std::max(peak, std::abs(s));
    WARN("duration " << info->durationSeconds << " s, " << info->averageBpm << " BPM, " << info->noteCount
                     << " notes, peak " << 20.0 * std::log10(peak) << " dBFS");
    CHECK(peak > 0.01f);
}
