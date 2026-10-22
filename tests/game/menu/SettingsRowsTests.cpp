#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/core/Types.h"
#include "engine/ui/TextPainter.h"

#include "FakeRenderDevice.h"
#include "game/menu/SettingsRows.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Fixture {
    BitmapFont font;
    test::FakeTexture texture{64, 64};
    TextPainter painter;
    MenuDefinition definition = MenuDefinition::parchment();
    Fixture() {
        std::vector<BitmapGlyph> glyphs;
        for (s32 c = ' '; c <= '~'; ++c) {
            glyphs.push_back({c, 8, 0, 0});
        }
        font = BitmapFont::fromGlyphs(32, 8, std::move(glyphs));
        painter.setFont(&font, &texture);
    }
    static std::string text(std::string_view id) {
        if (id == "settings.on") {
            return "On";
        }
        if (id == "settings.off") {
            return "Off";
        }
        return std::string(id);
    }
    SettingsRows rows() { return SettingsRows{definition, painter, &Fixture::text}; }
};

TEST_CASE("settings rows are a choice, a marked choice, a valued row or a switch",
          "[settings][settings-rows]") {
    Fixture f;
    auto rows = f.rows();
    rows.choice("Audio", 7);
    rows.marked("Easy", 1, false);
    rows.marked("Hard", 2, true);
    rows.value("Resolution", 3, "640 x 480", false);
    rows.toggle("Auto Save", 4, true);
    rows.toggle("Bloom", 5, false);
    const auto& items = f.definition.items;
    REQUIRE(items.size() == 6);
    CHECK(items[0].text == "Audio");
    CHECK(items[0].code == 7);
    CHECK(items[0].markedPart == 0);
    CHECK(items[0].value.empty());
    CHECK(items[0].enabled);
    CHECK(items[1].markedPart == 0);
    CHECK(items[2].markedPart == 1);
    CHECK(items[3].value == "640 x 480");
    CHECK_FALSE(items[3].enabled);
    CHECK(items[4].value == "On");
    CHECK(items[5].value == "Off");
    CHECK(items[5].enabled);
}

TEST_CASE("the choice column and the switch column are the pages' shared layout",
          "[settings][settings-rows]") {
    Fixture f;
    auto rows = f.rows();
    rows.column();
    CHECK(f.definition.x == SettingsRows::kColumnX);
    CHECK(f.definition.y == SettingsRows::kColumnY);
    CHECK(f.definition.scale == SettingsRows::kChoiceScale);
    CHECK(f.definition.cursorScale == SettingsRows::kChoiceScale);
    rows.column(78, 94, 0.45f);
    CHECK(f.definition.x == 78);
    CHECK(f.definition.y == 94);
    CHECK(f.definition.cursorScale == 0.45f);
    rows.switches();
    CHECK(f.definition.valueX == SettingsRows::kSwitchX);
    CHECK(f.definition.valueWidth == f.painter.measure("Off", 0.45f));
}

TEST_CASE("a fit keeps the longest row between the margins and no larger than asked",
          "[settings][settings-rows]") {
    Fixture f;
    auto rows = f.rows();
    rows.choice("Short", 0);
    rows.choice("A much longer choice", 1);
    // Twenty glyphs of eight: 160 wide at full size. 128 + 64 of margin leaves 320 of a
    // 512 screen, so the full size fits; a 300 screen leaves 108, two thirds of the row.
    rows.fit(1.0f, 512);
    CHECK(f.definition.scale == 1.0f);
    CHECK(f.definition.cursorScale == 1.0f);
    rows.fit(0.65f, 512);
    CHECK(f.definition.scale == 0.65f);
    rows.fit(1.0f, 300);
    CHECK(f.definition.scale == 108.0f / 160.0f);
    // A tail is room kept after every label, as for the mark beside a choice.
    rows.fit(1.0f, 300, " ~");
    CHECK(f.definition.scale == 108.0f / 176.0f);
    CHECK(f.definition.cursorScale == f.definition.scale);
}
} // namespace
