#include "game/screens/OrdinaryPickupPhase.h"

#include <array>

#include "game/players/ItemPickup.h"
#include "game/screens/PartyHelpAdmission.h"
#include "game/screens/PartyPickups.h"
#include "game/world/LevelWorldCheckpoint.h"

namespace gdl::game {

OrdinaryPickupPhase::Result OrdinaryPickupPhase::advance(std::span<PlayerRuntime> players,
                                                         LevelWorld& world, LevelFixtures& fixtures,
                                                         const ClassDataSet& classes,
                                                         LevelWorldCheckpoint& checkpoint) {
    if (!checkpoint.binds(world) || !fixtures.m_resources ||
        &fixtures.m_resources->world != &world) {
        return {.refusal = Refusal::Binding};
    }
    if (world.isTower() || world.ref().isSecret()) {
        return {.refusal = Refusal::Domain};
    }
    if (players.empty() || players.size() > 4) {
        return {.refusal = Refusal::Party};
    }
    std::array<bool, 4> occupied{};
    for (const auto& runtime : players) {
        const auto& actor = runtime.actor;
        const auto& save = actor.save();
        const s32 slot = actor.player();
        if (slot < 0 || static_cast<usize>(slot) >= occupied.size() ||
            occupied[static_cast<usize>(slot)] || save.character < 0 ||
            static_cast<usize>(save.character) >= save.classes.size()) {
            return {.refusal = Refusal::Party};
        }
        occupied[static_cast<usize>(slot)] = true;
    }

    Result result;
    const auto visitors = PartyPickups::collectors(players, world, fixtures.chests());
    const auto collected = checkpoint.collectItems(visitors, [&](const Pickup& pickup) {
        const auto refuse = [&](Refusal reason) {
            result.refusal = reason;
            result.refusedItem = pickup.item;
            return PickupDecision{.accepted = false};
        };
        if (pickup.realm > 0 || (pickup.subtype != static_cast<s32>(ItemKind::Gold) &&
                                 pickup.subtype != static_cast<s32>(ItemKind::Keys))) {
            if (PartyPickups::offerUnchanged(pickup, players, world, classes)) {
                return PickupDecision{};
            }
            return refuse(Refusal::Item);
        }
        return PartyPickups::takeCurrency(
            pickup, players, world, fixtures, classes,
            [&](const PartyPickups::CurrencyEvent& event) {
                using Kind = PartyPickups::CurrencyEvent::Kind;
                if (event.kind == Kind::Help) {
                    if (!PartyHelpAdmission::suppressed(event.lesson, event.recipient, players)) {
                        result.refusal = Refusal::Lesson;
                        result.refusedItem = pickup.item;
                        return false;
                    }
                    return true;
                }
                if (event.kind == Kind::ChallengeCoin) {
                    // PlayScene::open calls close (which clears the challenge),
                    // and beginChallenge only starts it in the secret realm.
                    // This native nonsecret scene invariant is checked above.
                    return true;
                }
                if (event.recipient >= players.size()) {
                    result.refusal = Refusal::Party;
                    result.refusedItem = pickup.item;
                    return false;
                }
                auto kind = Output::Kind::Card;
                if (event.kind == Kind::NamedSound) {
                    kind = Output::Kind::NamedSound;
                } else if (event.kind == Kind::CharacterBark) {
                    kind = Output::Kind::CharacterBark;
                }
                result.outputs.push_back(
                    {kind, pickup.item, players[event.recipient].actor.player(), event.name});
                return true;
            });
    });
    if (!collected) {
        return {.refusal = result.refusal == Refusal::None ? Refusal::Burst : result.refusal,
                .refusedItem = result.refusedItem};
    }
    PartyPickups::cleanupCollectedChests(world, fixtures.chests());
    result.advanced = true;
    result.collected = collected->size();
    return result;
}

} // namespace gdl::game
