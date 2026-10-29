#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "engine/platform/Paths.h"

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep
#else
#include <cstdlib>
#endif

namespace {

using namespace gdl;

TEST_CASE("executableDirectory is the directory holding the test binary", "[platform][paths]") {
    const std::filesystem::path dir = paths::executableDirectory();
    REQUIRE(dir.is_absolute());
    REQUIRE(std::filesystem::is_directory(dir));
    const bool hasBinary =
        std::filesystem::exists(dir / "tests.exe") || std::filesystem::exists(dir / "tests");
    REQUIRE(hasBinary);
}

TEST_CASE("paths carry every character as UTF-8 and back", "[platform][paths][unicode]") {
    // "sävé-日本", as the bytes a settings file or a command line holds.
    const std::string text = "s\xC3\xA4v\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC";
    const std::filesystem::path path = paths::fromUtf8(text);
    CHECK(path == std::filesystem::path(u8"s\u00E4v\u00E9-\u65E5\u672C"));
    CHECK(paths::utf8(path) == text);
    CHECK(paths::utf8(std::filesystem::path("plain") / "name.txt").ends_with("name.txt"));
    CHECK(paths::genericUtf8(std::filesystem::path("a") / "b") == "a/b");
    CHECK(paths::fromUtf8("").empty());
}

TEST_CASE("an environment variable comes through as a path whole", "[platform][paths][unicode]") {
#ifdef _WIN32
    REQUIRE(SetEnvironmentVariableW(L"GDL_TEST_PATH", L"C:\\s\u00E4v\u00E9-\u65E5\u672C") != 0);
#else
    REQUIRE(setenv("GDL_TEST_PATH", "/s\xC3\xA4v\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC", 1) == 0);
#endif
    const std::filesystem::path value = paths::environment("GDL_TEST_PATH");
    CHECK(paths::utf8(value.filename()) == "s\xC3\xA4v\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC");
    CHECK(paths::environment("GDL_TEST_PATH_NEVER_SET").empty());
}

} // namespace
