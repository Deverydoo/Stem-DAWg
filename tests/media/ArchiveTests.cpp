// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// .tar.gz extraction (installing the voice model).

#include "TempDir.h"
#include "TestTarGz.h"

#include <daw/core/Files.h>
#include <daw/media/TarGzArchive.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using namespace daw;
using namespace daw::test;

namespace {

std::string repeat(const std::string& text, int times)
{
    std::string out;
    for (int i = 0; i < times; ++i)
        out += text;
    return out;
}

} // namespace

TEST_CASE("Named files are extracted from a .tar.gz, flat and all or nothing", "[media][archive]")
{
    TempDir dir("targz");
    const auto archive = dir / "model.tar.gz";
    REQUIRE(writeFileAtomically(archive, std::string(reinterpret_cast<const char*>(kTestTarGz), sizeof kTestTarGz)).ok());

    const auto out = dir / "model";
    REQUIRE(extractTarGzFiles(archive, {"enc.onnx", "erb_dec.onnx", "df_dec.onnx"}, out).ok());
    CHECK(readFileBytes(out / "enc.onnx").value() == repeat("encoder payload\n", 50));
    CHECK(readFileBytes(out / "erb_dec.onnx").value() == repeat("erb decoder payload\n", 40));
    CHECK(readFileBytes(out / "df_dec.onnx").value() == repeat("df decoder payload\n", 30)); // behind a GNU long name
    CHECK_FALSE(std::filesystem::exists(out / "config.ini")); // only what was asked for
    CHECK_FALSE(std::filesystem::exists(out / "enc.onnx.part"));

    // A missing name: nothing is written.
    const auto missing = extractTarGzFiles(archive, {"enc.onnx", "absent.onnx"}, dir / "none");
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK_FALSE(std::filesystem::exists(dir / "none" / "enc.onnx"));

    // Over the size limit.
    CHECK(extractTarGzFiles(archive, {"enc.onnx"}, dir / "small", 1000).error().code == ErrorCode::LimitExceeded);

    // Not an archive, and a damaged one.
    REQUIRE(writeFileAtomically(dir / "plain.txt", std::string("hello")).ok());
    CHECK_FALSE(extractTarGzFiles(dir / "plain.txt", {"enc.onnx"}, dir / "p").ok());
    std::string damaged(reinterpret_cast<const char*>(kTestTarGz), sizeof kTestTarGz);
    damaged.resize(damaged.size() / 2);
    REQUIRE(writeFileAtomically(dir / "damaged.tar.gz", damaged).ok());
    CHECK_FALSE(extractTarGzFiles(dir / "damaged.tar.gz", {"enc.onnx", "erb_dec.onnx", "df_dec.onnx"}, dir / "d").ok());
    CHECK(extractTarGzFiles(dir / "absent.tar.gz", {"enc.onnx"}, dir / "a").error().code == ErrorCode::NotFound);
}
