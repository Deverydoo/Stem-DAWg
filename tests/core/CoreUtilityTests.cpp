// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/core/CancellationToken.h>
#include <daw/core/Files.h>
#include <daw/core/Hash.h>
#include <daw/core/Ids.h>
#include <daw/core/Logger.h>
#include <daw/core/Result.h>
#include <daw/core/RtThread.h>

#include "TempDir.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <set>

using namespace daw;

TEST_CASE("Generated UUIDs are canonical v4 and unique", "[core][ids]")
{
    std::set<std::string> seen;
    for (int i = 0; i < 2000; ++i) {
        const std::string id = generateUuid();
        REQUIRE(isCanonicalUuid(id));
        REQUIRE(id[14] == '4');
        REQUIRE(std::string("89ab").find(id[19]) != std::string::npos);
        REQUIRE(seen.insert(id).second);
    }
    REQUIRE_FALSE(isCanonicalUuid("not-a-uuid"));
}

TEST_CASE("Typed ids compare by value", "[core][ids]")
{
    const TrackId a("x");
    const TrackId b("x");
    REQUIRE(a == b);
    REQUIRE_FALSE(TrackId().valid());
    REQUIRE(TrackId::generate() != TrackId::generate());
}

TEST_CASE("Result carries values and errors", "[core][result]")
{
    Result<int> good = 42;
    REQUIRE(good.ok());
    REQUIRE(*good == 42);

    Result<int> bad = makeError(ErrorCode::NotFound, "missing");
    REQUIRE_FALSE(bad.ok());
    REQUIRE(bad.error().code == ErrorCode::NotFound);
    REQUIRE(bad.error().describe() == "not-found: missing");

    Result<std::unique_ptr<int>> moved = std::make_unique<int>(7);
    REQUIRE(**moved == 7);

    Status ok;
    REQUIRE(ok.ok());
    Status failed = makeError(ErrorCode::IoError, "disk");
    REQUIRE_FALSE(failed.ok());
}

TEST_CASE("CancellationSource propagates to tokens", "[core][cancel]")
{
    CancellationSource source;
    const CancellationToken token = source.token();
    REQUIRE_FALSE(token.isCancelled());
    source.cancel();
    REQUIRE(token.isCancelled());
    REQUIRE_FALSE(CancellationToken{}.isCancelled());
}

TEST_CASE("FNV-1a matches reference vectors", "[core][hash]")
{
    REQUIRE(fnv1a64("") == 0xcbf29ce484222325ull);
    REQUIRE(fnv1a64("a") == 0xaf63dc4c8601ec8cull);
}

TEST_CASE("Atomic file writes replace content and handle Unicode paths", "[core][files]")
{
    test::TempDir dir("files");
    const auto path = dir.path() / utf8ToPath("Ünïcødé ✓") / "data.bin";
    REQUIRE(writeFileAtomically(path, "first").ok());
    REQUIRE(*readFileBytes(path) == "first");
    REQUIRE(writeFileAtomically(path, std::string(100000, 'x')).ok());
    REQUIRE(readFileBytes(path)->size() == 100000);

    // No temporary files are left behind.
    int entries = 0;
    for ([[maybe_unused]] const auto& e : std::filesystem::directory_iterator(path.parent_path()))
        ++entries;
    REQUIRE(entries == 1);

    REQUIRE(readFileBytes(dir.path() / "missing").error().code == ErrorCode::NotFound);
    REQUIRE(pathToUtf8(utf8ToPath("Ünïcødé ✓")) == "Ünïcødé ✓");
}

TEST_CASE("Timestamps have the documented shapes", "[core][files]")
{
    const std::string iso = utcTimestampIso8601();
    REQUIRE(iso.size() == 24);
    REQUIRE(iso.back() == 'Z');
    REQUIRE(utcTimestampForFilename().size() == 19);
}

TEST_CASE("Logger delivers structured records and refuses the audio thread", "[core][log]")
{
    std::atomic<int> received{0};
    std::string lastLine;
    const int sink = Logger::instance().addSink([&](const LogRecord& record) {
        if (record.category == "test") {
            lastLine = record.toJsonLine();
            ++received;
        }
    });

    logging::info("test", "hello", {{"answer", 42}});
    Logger::instance().flush();
    REQUIRE(received == 1);
    REQUIRE(lastLine.find("\"answer\":42") != std::string::npos);
    REQUIRE(lastLine.find("\"msg\":\"hello\"") != std::string::npos);

    const auto droppedBefore = Logger::instance().droppedFromAudioThread();
    {
        rt::ScopedAudioThread audio;
        logging::info("test", "from audio thread");
    }
    Logger::instance().flush();
    REQUIRE(received == 1);
    REQUIRE(Logger::instance().droppedFromAudioThread() == droppedBefore + 1);

    Logger::instance().removeSink(sink);
}
