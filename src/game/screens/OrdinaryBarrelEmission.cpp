#include "game/screens/OrdinaryBarrelEmission.h"

#include "game/screens/LevelFixtures.h"
#include "game/screens/PartyHelpAdmission.h"
#include "game/screens/PlayerRuntime.h"
#include "game/world/LevelWorldCheckpoint.h"

namespace gdl::game {

OrdinaryBarrelEmission::Result OrdinaryBarrelEmission::advance(const BreakableStrike& strike,
                                                               std::string_view sound, s32 byPlayer,
                                                               std::span<PlayerRuntime> players,
                                                               LevelFixtures& fixtures,
                                                               LevelWorldCheckpoint& world) {
    const auto refuse = [](Refusal reason) { return Result{.refusal = reason}; };
    if (!fixtures.m_resources || !world.binds(fixtures.m_resources->world) ||
        strike.index >= fixtures.barrels().size() ||
        strike.kind != fixtures.barrels().barrel(strike.index).kind) {
        return refuse(Refusal::Binding);
    }
    if (strike.broken && strike.kind != BreakableStrike::Kind::Plain &&
        strike.kind != BreakableStrike::Kind::Holding) {
        return refuse(Refusal::Kind);
    }
    Result result{
        .accepted = true,
        .cue = PlayerArsenal::ImpactCue{std::string{sound}, strike.position, std::nullopt, 1}};
    if (!strike.broken || strike.contents < 0) {
        return result;
    }

    const auto& records = fixtures.m_resources->world.layout().itemInfos();
    if (static_cast<usize>(strike.contents) >= records.size() ||
        records[static_cast<usize>(strike.contents)].type != ItemInfo::kPowerup) {
        // Native releaseEnemy precedes pickup birth. Until that phase is
        // supported, do not reinterpret a placed enemy as an empty pickup.
        return refuse(Refusal::Record);
    }

    bool lesson = false;
    const LevelFixtures::Events events{.help = [&](s32 id, usize index) {
        lesson |= !PartyHelpAdmission::suppressed(id, index, players);
        return false;
    }};
    const bool accepted = fixtures.releaseBarrelContents(
        strike, byPlayer, players, events,
        [&](s32 record, const Vec3& position, s32 count) -> std::optional<bool> {
            result.pickup = world.placeItemRecord(record, position, count);
            return result.pickup ? std::optional{true} : std::nullopt;
        });
    if (!accepted) {
        return refuse(Refusal::Birth);
    }
    if (lesson) {
        // The native sequence births the item before asking for help. Keep that
        // order; the encompassing undo also removes this late-refused birth.
        return refuse(Refusal::Lesson);
    }
    return result;
}

} // namespace gdl::game
