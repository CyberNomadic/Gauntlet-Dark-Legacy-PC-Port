#include <set>

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/EffectIdentity.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("strike effect identities distinguish action chains without pool handles",
          "[netplay][effect-identity]") {
    const EffectIdentity origin{1, 0, 3, 4, 0, 0, 0};
    REQUIRE(origin.valid());
    std::set<EffectIdentity> identities{origin};
    for (usize part = 0; part < 7; ++part) {
        auto other = origin;
        switch (part) {
        case 0: ++other.epoch; break;
        case 1: ++other.tick; break;
        case 2: ++other.generation; break;
        case 3: ++other.grant; break;
        case 4: ++other.seat; break;
        case 5: ++other.occurrence; break;
        default: ++other.effectOrdinal; break;
        }
        CHECK(other.valid());
        CHECK(other != origin);
        CHECK(identities.insert(other).second);
    }
    CHECK(identities.size() == 8);
    auto maximum = origin;
    maximum.seat = 3;
    maximum.occurrence = 127;
    maximum.effectOrdinal = 127;
    CHECK(maximum.valid());
    for (usize part = 0; part < 6; ++part) {
        auto invalid = origin;
        switch (part) {
        case 0: invalid.epoch = 0; break;
        case 1: invalid.generation = 0; break;
        case 2: invalid.grant = 0; break;
        case 3: invalid.seat = 4; break;
        case 4: invalid.occurrence = 128; break;
        default: invalid.effectOrdinal = 128; break;
        }
        CHECK_FALSE(invalid.valid());
    }
}
} // namespace
