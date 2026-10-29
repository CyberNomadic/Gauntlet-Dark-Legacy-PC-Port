#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"

#include "TestSupport.h"

namespace {

using namespace gdl;

auto nativeContent(const BitmapFont& font) {
    NativeStateDigest digest;
    digest.value(font);
    REQUIRE(digest.good());
    return digest.finish();
}

TEST_CASE("font content identity includes hidden glyph metrics and invalidates on replacement",
          "[assets][font][native-text-digest]") {
    auto a = BitmapFont::fromGlyphs(10, 4, {{'A', 6, 0, 0}, {'B', 8, 6, 0}});
    auto b = BitmapFont::fromGlyphs(10, 4, {{'A', 6, 0, 0}, {'B', 8, 6, 0}});
    const auto initial = nativeContent(a);
    CHECK(nativeContent(b) == initial);
    b = BitmapFont::fromGlyphs(10, 4, {{'A', 6, 0, 0}, {'B', 9, 6, 0}});
    CHECK(nativeContent(b) != initial);
    b = BitmapFont::fromGlyphs(10, 5, {{'A', 6, 0, 0}, {'B', 8, 6, 0}});
    CHECK(nativeContent(b) != initial);
    b = BitmapFont::fromGlyphs(11, 4, {{'A', 6, 0, 0}, {'B', 8, 6, 0}});
    CHECK(nativeContent(b) != initial);
    b = BitmapFont::fromGlyphs(10, 4, {{'A', 6, 0, 0}, {'B', 8, 7, 0}});
    CHECK(nativeContent(b) != initial);
    const auto dir = test::scratchDirectory("font-native-content");
    REQUIRE_FALSE(a.load(dir / "missing.json", 4));
    CHECK(nativeContent(a) == nativeContent(BitmapFont::fromGlyphs(0, 4, {})));
    CHECK(nativeContent(a) != initial);
}

TEST_CASE("glyphs are found by character code", "[assets][font]") {
    const BitmapFont font =
        BitmapFont::fromGlyphs(10, 4, {{'A', 6, 0, 0}, {'B', 8, 6, 0}, {'A', 1, 1, 1}});
    REQUIRE(font.loaded());
    REQUIRE(font.height() == 10);
    REQUIRE(font.spaceWidth() == 4);
    REQUIRE(font.glyphCount() == 3);
    REQUIRE(font.glyph('A') != nullptr);
    REQUIRE(font.glyph('A')->width == 6);
    REQUIRE(font.glyph('B')->x == 6);
    REQUIRE(font.glyph('C') == nullptr);
    REQUIRE(font.glyph(0) == nullptr);
}

TEST_CASE("a glyph is cut from part of another's cell and set within its own", "[assets][font]") {
    BitmapFont font = BitmapFont::fromGlyphs(10, 4, {{'A', 6, 0, 0}, {'E', 8, 6, 0}});
    const auto before = nativeContent(font);
    BitmapFont::Cut cut{.code = '-',
                        .from = 'E',
                        .left = 2,
                        .top = 4,
                        .columns = 5,
                        .rows = 3,
                        .width = 7,
                        .lead = 1,
                        .at = 5};
    REQUIRE(font.addCut(cut));
    const BitmapGlyph* hyphen = font.glyph('-');
    REQUIRE(hyphen != nullptr);
    CHECK(hyphen->x == 8);
    CHECK(hyphen->y == 4);
    CHECK(hyphen->columns == 5);
    CHECK(hyphen->rows == 3);
    CHECK(hyphen->width == 7);
    CHECK(hyphen->lead == 1);
    CHECK(hyphen->top == 5);
    CHECK(font.glyphCount() == 3);
    CHECK(nativeContent(font) != before); // the content identity sees the new cell
    // A character the font has is kept; a part outside either cell is refused.
    CHECK_FALSE(font.addCut(cut));
    cut.code = '=';
    cut.from = 'Z';
    CHECK_FALSE(font.addCut(cut));
    cut.from = 'E';
    cut.left = 4; // 4 + 5 columns pass the E's 8
    CHECK_FALSE(font.addCut(cut));
    cut.left = 2;
    cut.at = 8; // 8 + 3 rows pass the font's 10
    CHECK_FALSE(font.addCut(cut));
    cut.at = 5;
    cut.width = 5; // the lead and the columns need 6
    CHECK_FALSE(font.addCut(cut));
    cut.width = 7;
    CHECK(font.addCut(cut));
    CHECK(font.glyph('=') != nullptr);
    BitmapFont empty;
    CHECK_FALSE(empty.addCut(cut));
}

TEST_CASE("font manifests load from disk", "[assets][font]") {
    const auto dir = test::scratchDirectory("bitmap-font");
    writeTextFile(dir / "small.json",
                  R"({"height": 12, "glyphs": [{"code": 65, "width": 7, "x": 10, "y": 20}]})");
    BitmapFont font;
    REQUIRE(font.load(dir / "small.json", 5));
    REQUIRE(font.height() == 12);
    REQUIRE(font.spaceWidth() == 5);
    REQUIRE(font.glyph('A')->y == 20);

    writeTextFile(dir / "bad.json", R"({"height": 0, "glyphs": []})");
    REQUIRE_FALSE(font.load(dir / "bad.json", 5));
    REQUIRE_FALSE(font.loaded());
    REQUIRE_FALSE(font.load(dir / "missing.json", 5));
}

TEST_CASE("the native menu font has 32 pixel glyphs", "[assets][font]") {
    const auto file = test::assetOrSkip("FONTS/font32.fnt");
    BitmapFont font;
    REQUIRE(font.load(file, 16));
    REQUIRE(font.height() == 32);
    REQUIRE(font.glyph('P') != nullptr);
    REQUIRE(font.glyph('P')->width == 24);
    REQUIRE(font.glyph(' ') == nullptr);
}

} // namespace
