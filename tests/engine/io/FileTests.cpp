#include <array>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"

namespace {

using namespace gdl;

TEST_CASE("files round-trip through writeFile and readFile", "[io][file]") {
    const auto dir = test::scratchDirectory("file-roundtrip");
    const std::vector<u8> payload{1, 2, 3, 250, 251, 252};
    writeFile(dir / "data.bin", payload);
    REQUIRE(readFile(dir / "data.bin") == payload);
}

TEST_CASE("FileStream seeks and reads", "[io][file]") {
    const auto dir = test::scratchDirectory("file-stream");
    std::vector<u8> payload(100);
    for (usize i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<u8>(i);
    }
    writeFile(dir / "stream.bin", payload);

    FileStream stream(dir / "stream.bin");
    REQUIRE(stream.size() == 100);
    REQUIRE(stream.position() == 0);

    std::array<u8, 4> head{};
    REQUIRE(stream.read(head) == 4);
    REQUIRE(head == std::array<u8, 4>{0, 1, 2, 3});
    REQUIRE(stream.position() == 4);

    stream.seek(96);
    const auto tail = stream.readExact(4);
    REQUIRE(tail == std::vector<u8>{96, 97, 98, 99});
    REQUIRE(stream.position() == 100);

    std::array<u8, 4> beyond{};
    REQUIRE(stream.read(beyond) == 0);
    REQUIRE_THROWS_AS(stream.readExact(1), FileError);
    REQUIRE_THROWS_AS(stream.seek(101), FileError);
}

TEST_CASE("missing files throw FileError", "[io][file]") {
    const auto dir = test::scratchDirectory("file-missing");
    REQUIRE_THROWS_AS(readFile(dir / "nope.bin"), FileError);
    REQUIRE_THROWS_AS(FileStream(dir / "nope.bin"), FileError);
}

TEST_CASE("transactional text writes replace complete files and clean up failed replacements",
          "[io][file]") {
    const auto dir = test::scratchDirectory("file-replace");
    const auto path = dir / "save.json";
    replaceTextFile(path, "old character");
    replaceTextFile(path, "new character");
    CHECK(readTextFile(path) == "new character");
    CHECK(std::distance(std::filesystem::directory_iterator(dir),
                        std::filesystem::directory_iterator{}) == 1);
    const auto blocked = dir / "occupied";
    std::filesystem::create_directory(blocked);
    writeTextFile(blocked / "keep", "unrelated");
    REQUIRE_THROWS_AS(replaceTextFile(blocked, "replacement"), FileError);
    CHECK(readTextFile(blocked / "keep") == "unrelated");
    CHECK(readTextFile(path) == "new character");
    CHECK(std::distance(std::filesystem::directory_iterator(dir),
                        std::filesystem::directory_iterator{}) == 2);
}

TEST_CASE("text files read back as strings", "[io][file]") {
    const auto dir = test::scratchDirectory("file-text");
    writeTextFile(dir / "note.txt", "hello\nworld");
    REQUIRE(readTextFile(dir / "note.txt") == "hello\nworld");
    REQUIRE_THROWS_AS(readTextFile(dir / "missing.txt"), FileError);
}

TEST_CASE("files under a name beyond the system code page read, write and report",
          "[io][file][unicode]") {
    const auto dir = test::scratchDirectory("file-unicode") /
                     std::filesystem::path(u8"s\u00E4v\u00E9-\u65E5\u672C");
    std::filesystem::create_directories(dir);
    const std::vector<u8> payload{7, 8, 9};
    writeFile(dir / "data.bin", payload);
    CHECK(readFile(dir / "data.bin") == payload);
    writeTextFile(dir / "text.txt", "one");
    replaceTextFile(dir / "text.txt", "two");
    CHECK(readTextFile(dir / "text.txt") == "two");
    const FileStream stream(dir / "data.bin");
    CHECK(stream.size() == 3);
    // A missing file is named in UTF-8 by the error, never a code page complaint.
    try {
        readFile(dir / "missing.bin");
        FAIL("a missing file must throw");
    } catch (const FileError& error) {
        const std::string_view what{error.what()};
        CHECK(what.find("s\xC3\xA4v\xC3\xA9") != std::string_view::npos);
        CHECK(what.find("missing.bin") != std::string_view::npos);
    }
}

} // namespace
