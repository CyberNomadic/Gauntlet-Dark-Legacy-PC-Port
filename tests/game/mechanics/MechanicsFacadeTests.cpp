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
constexpr std::array kOutcomes{BlowOutcome::Hurt, BlowOutcome::Killed, BlowOutcome::Destroyed};

TEST_CASE("out of Aggressor Mode the original's rules stand: swings alone count, as the original "
          "counted them, and anything harms a generator",
          "[game][mechanics][aggressor]") {
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    const MechanicsFacade off{{.aggressorMode = false}};
    CHECK_FALSE(off.aggressorMode());
    CHECK_FALSE(off.meterShown());
    CHECK_FALSE(off.swarmAggravated());
    for (const BlowSource source : kSources) {
        for (const BlowOutcome outcome : kOutcomes) {
            CAPTURE(static_cast<int>(source), static_cast<int>(outcome));
            const bool original =
                source == BlowSource::Swing && outcome != BlowOutcome::Destroyed;
            CHECK(off.runEffect(source, outcome, players[0], players) ==
                  (original ? RunEffect::CountAsOriginal : RunEffect::Nothing));
        }
        CHECK(off.generatorTakes(source));
    }
}

TEST_CASE("in Aggressor Mode a kill or a generator destroyed counts one, any other blow refreshes "
          "the run, and only swings and shots harm a generator",
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
        CHECK(on.runEffect(source, BlowOutcome::Hurt, players[0], players) == RunEffect::Refresh);
        CHECK(on.runEffect(source, BlowOutcome::Killed, players[0], players) == RunEffect::Count);
        CHECK(on.runEffect(source, BlowOutcome::Destroyed, players[0], players) ==
              RunEffect::Count);
        CHECK(on.generatorTakes(source) ==
              (source == BlowSource::Swing || source == BlowSource::Shot));
    }
    MechanicsFacade switched;
    switched.configure({.aggressorMode = false});
    CHECK_FALSE(switched.aggressorMode());
    CHECK(switched.settings() == MechanicsFacade::Settings{.aggressorMode = false});
}

TEST_CASE("blows struck under time stop, invulnerability or invisibility do nothing for the run",
          "[game][mechanics][aggressor]") {
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    players[1].actor.spawn(1, {}, nullptr, Vec3{0, 0, 5}, 0);
    const MechanicsFacade on;
    REQUIRE(on.runEffect(BlowSource::Shot, BlowOutcome::Killed, players[0], players) ==
            RunEffect::Count);
    REQUIRE(on.runEffect(BlowSource::Shot, BlowOutcome::Hurt, players[1], players) ==
            RunEffect::Refresh);
    // Invulnerable, the first earns nothing; the second is not affected.
    players[0].actor.save().progress().inventory.addPowerup(powerup::kArmor,
                                                            powerup::kInvulnerable, 0, 1);
    CHECK(on.runEffect(BlowSource::Swing, BlowOutcome::Killed, players[0], players) ==
          RunEffect::Nothing);
    CHECK(on.runEffect(BlowSource::Swing, BlowOutcome::Hurt, players[0], players) ==
          RunEffect::Nothing);
    CHECK(on.runEffect(BlowSource::Swing, BlowOutcome::Killed, players[1], players) ==
          RunEffect::Count);
    // Invisible, the second earns nothing either.
    players[1].actor.save().progress().inventory.addPowerup(powerup::kSpecial, powerup::kInvisible,
                                                            0, 1);
    CHECK(on.runEffect(BlowSource::Turbo, BlowOutcome::Killed, players[1], players) ==
          RunEffect::Nothing);
    // Time stopped by anyone, nobody's blows do anything.
    std::array<PlayerRuntime, 2> stopped;
    stopped[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    stopped[1].actor.spawn(1, {}, nullptr, Vec3{0, 0, 5}, 0);
    stopped[1].actor.save().progress().inventory.addPowerup(powerup::kSpecial, powerup::kStopTime,
                                                            0, 1);
    CHECK(on.runEffect(BlowSource::Swing, BlowOutcome::Killed, stopped[0], stopped) ==
          RunEffect::Nothing);
    CHECK(on.runEffect(BlowSource::Swing, BlowOutcome::Hurt, stopped[1], stopped) ==
          RunEffect::Nothing);
    // Out of the mode the original's swing counts regardless.
    const MechanicsFacade off{{.aggressorMode = false}};
    CHECK(off.runEffect(BlowSource::Swing, BlowOutcome::Hurt, stopped[1], stopped) ==
          RunEffect::CountAsOriginal);
}

} // namespace
