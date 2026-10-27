#include <array>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/mechanics/MechanicsFacade.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/PowerupEffects.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr std::array kSources{BlowSource::Swing,  BlowSource::Shot,   BlowSource::SuperShot,
                              BlowSource::Potion, BlowSource::Turbo,  BlowSource::Charge,
                              BlowSource::Breath, BlowSource::Shield};

TEST_CASE("out of Aggressor Mode the original's rules stand: swings alone build the run and "
          "anything harms a generator",
          "[game][mechanics][aggressor]") {
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    const MechanicsFacade off{{.aggressorMode = false}};
    CHECK_FALSE(off.aggressorMode());
    CHECK_FALSE(off.meterShown());
    CHECK_FALSE(off.swarmAggravated());
    for (const BlowSource source : kSources) {
        CAPTURE(static_cast<int>(source));
        CHECK(off.blowBuildsRun(source, players[0], players) == (source == BlowSource::Swing));
        CHECK(off.generatorTakes(source));
    }
}

TEST_CASE("in Aggressor Mode every blow builds the run and only swings and shots harm a "
          "generator",
          "[game][mechanics][aggressor]") {
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    const MechanicsFacade on;
    CHECK(on.aggressorMode());
    CHECK(on.meterShown());
    CHECK(on.swarmAggravated());
    CHECK(MechanicsFacade::standard().aggressorMode());
    for (const BlowSource source : kSources) {
        CAPTURE(static_cast<int>(source));
        CHECK(on.blowBuildsRun(source, players[0], players));
        CHECK(on.generatorTakes(source) ==
              (source == BlowSource::Swing || source == BlowSource::Shot));
    }
    MechanicsFacade switched;
    switched.configure({.aggressorMode = false});
    CHECK_FALSE(switched.aggressorMode());
    CHECK(switched.settings() == MechanicsFacade::Settings{.aggressorMode = false});
}

TEST_CASE("blows struck under time stop, invulnerability or invisibility build no run",
          "[game][mechanics][aggressor]") {
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    players[1].actor.spawn(1, {}, nullptr, Vec3{0, 0, 5}, 0);
    const MechanicsFacade on;
    REQUIRE(on.blowBuildsRun(BlowSource::Shot, players[0], players));
    REQUIRE(on.blowBuildsRun(BlowSource::Shot, players[1], players));
    // Invulnerable, the first builds nothing; the second is not affected.
    players[0].actor.save().progress().inventory.addPowerup(powerup::kArmor,
                                                            powerup::kInvulnerable, 0, 1);
    CHECK_FALSE(on.blowBuildsRun(BlowSource::Swing, players[0], players));
    CHECK(on.blowBuildsRun(BlowSource::Swing, players[1], players));
    // Invisible, the second builds nothing either.
    players[1].actor.save().progress().inventory.addPowerup(powerup::kSpecial, powerup::kInvisible,
                                                            0, 1);
    CHECK_FALSE(on.blowBuildsRun(BlowSource::Turbo, players[1], players));
    // Time stopped by anyone, nobody's blows build a run.
    std::array<PlayerRuntime, 2> stopped;
    stopped[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    stopped[1].actor.spawn(1, {}, nullptr, Vec3{0, 0, 5}, 0);
    stopped[1].actor.save().progress().inventory.addPowerup(powerup::kSpecial, powerup::kStopTime,
                                                            0, 1);
    CHECK_FALSE(on.blowBuildsRun(BlowSource::Swing, stopped[0], stopped));
    CHECK_FALSE(on.blowBuildsRun(BlowSource::Swing, stopped[1], stopped));
    // Out of the mode the original's swing counts regardless.
    const MechanicsFacade off{{.aggressorMode = false}};
    CHECK(off.blowBuildsRun(BlowSource::Swing, stopped[1], stopped));
}

} // namespace
