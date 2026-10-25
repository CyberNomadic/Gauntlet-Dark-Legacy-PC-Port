#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/scenes/SceneSelection.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("a selection's token names its kind and index, and reads back in any case",
          "[dev-console][selection]") {
    const SceneSelection object{SceneSelection::Kind::Object, 12, "K4XS#66"};
    CHECK(object.token() == "object#12");
    CHECK(object.label() == "object#12 K4XS#66");
    CHECK(SceneSelection::kindName(SceneSelection::Kind::Generator) == "generator");
    const auto parsed = SceneSelection::parse("Object#12");
    REQUIRE(parsed.has_value());
    CHECK(parsed->kind == SceneSelection::Kind::Object);
    CHECK(parsed->id == 12);
    CHECK(parsed->name.empty());
    CHECK(parsed->label() == "object#12");
    for (const auto kind : {SceneSelection::Kind::Enemy, SceneSelection::Kind::Critter,
                            SceneSelection::Kind::Generator, SceneSelection::Kind::Pickup,
                            SceneSelection::Kind::Player, SceneSelection::Kind::Spawned}) {
        const SceneSelection what{kind, 3, ""};
        const auto back = SceneSelection::parse(what.token());
        REQUIRE(back.has_value());
        CHECK(*back == what);
    }
    // No kind, no index, a sign, letters in the index or nothing after the hash: not a token.
    CHECK_FALSE(SceneSelection::parse("#12").has_value());
    CHECK_FALSE(SceneSelection::parse("object#").has_value());
    CHECK_FALSE(SceneSelection::parse("object#-1").has_value());
    CHECK_FALSE(SceneSelection::parse("object#1x").has_value());
    CHECK_FALSE(SceneSelection::parse("wall#1").has_value());
    CHECK_FALSE(SceneSelection::parse("object12").has_value());
}

} // namespace
