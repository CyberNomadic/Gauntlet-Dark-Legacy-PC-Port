// Extended local diagnostics only. Production sessions use the direct live
// watchdog and never capture or transfer these represented checkpoint states.
#include "game/screens/SimulationObserver.h"

#include <memory>
#include <utility>

#include "game/netplay/CanonicalDigestSink.h"
#include "game/netplay/EnemyMissilesCheckpoint.h"
#include "game/netplay/EnemyPoolCheckpoint.h"
#include "game/netplay/GeneratorCheckpoint.h"
#include "game/netplay/PlacedEnemyCheckpoint.h"
#include "game/screens/OpponentResources.h"

namespace gdl::game {
namespace {
using Subsystem = SimulationObservation::Subsystem;

Subsystem unavailable(std::string name, std::string reason) {
    return {std::move(name), std::nullopt, std::move(reason)};
}
Subsystem observed(std::string name, std::optional<SimulationObservation::Digest> digest,
                   std::string reason) {
    if (!digest) {
        return unavailable(std::move(name), std::move(reason));
    }
    return {std::move(name), std::move(digest), {}};
}
template <class Codec, class State>
Subsystem captured(std::string name, const std::optional<State>& state) {
    if (!state) {
        return unavailable(std::move(name), "native checkpoint refused current state or binding");
    }
    return observed(std::move(name), Codec::canonicalDigest(*state),
                    "canonical field walker refused its record or byte bounds");
}

} // namespace

class SimulationCheckpointObserver {
public:
    static std::optional<OpponentCombatCheckpoint::State>
    adapterValues(const OpponentCombatCheckpoint& adapter,
                  const EffectTrees::PoolCheckpoint& effects) {
        return adapter.captureValues(effects);
    }
};
SimulationObservation SimulationObserver::observe() {
    if (m_generation != m_scene.m_openGeneration) {
        clear();
    }
    SimulationObservation result;
    result.unobserved = {
        "PartyHud: selectors, help/message queues, pickup cards, hourglass and relic HUD clocks",
        "PartyPickups/PickupVoices: retained voice-selection random state",
        "LevelArrivalPresentation/StartCamera: hidden hold/ride/title clocks and spawn particles",
        "TowerWelcome, SumnerVisit/SumnerHints, SumnerFigure, TowerPromotion and TowerRelics: "
        "hidden clocks, queues, figures and ceremony effects",
        "LevelMessages/ScrollBox, SwitchCutscene, GameOver: hidden clocks and pending cues",
        "SecretChallenge: exact remaining coin IDs (counts/clock only observed)",
        "AmbientDimmer and CameraShake: hidden target/hold/fractional clocks",
        "Bosses, BossSequence/LegendRite/BossMeters, BossCamera and SkorneArena owners",
        "LevelOpponents Death/yell playback handles and their external audio state",
        "LevelSoundscape music/ambience, TriggerCues and external sound/mixer playback, "
        "callbacks and output queues (logical speech timing is observed)",
        "Render-history inputs (presented clip/previous camera) and retained promotion figures",
        "Application travel/shop/secret-parent ownership, persistence/unlock callbacks, config "
        "and input-source state outside the sampled scene scalars",
    };
    result.subsystems.reserve(7);

    const auto combat = [&]() -> Subsystem {
        if (!m_scene.m_open) {
            return unavailable("party_combat", "scene is closed");
        }
        if (!m_combatResources) {
            return unavailable("party_combat", "load-time combat resource catalog is not bound");
        }
        if (!m_combatResources->matches(m_scene.m_players)) {
            return unavailable("party_combat", "combat resource catalog no longer matches party");
        }
        std::vector<std::unique_ptr<PlayerRuntimeCheckpoint>> owners;
        std::vector<PlayerRuntimeCheckpoint*> players;
        owners.reserve(m_scene.m_players.size());
        players.reserve(m_scene.m_players.size());
        for (auto& player : m_scene.m_players) {
            owners.push_back(std::make_unique<PlayerRuntimeCheckpoint>(
                player, m_scene.m_classes.stats(player.actor.save().character)));
            players.push_back(owners.back().get());
        }
        CombatRollbackCheckpoint capture{players,
                                         m_scene.m_health,
                                         m_scene.m_levels,
                                         m_scene.m_attacks,
                                         m_scene.m_arsenal,
                                         m_scene.m_effects,
                                         m_combatResources->resources()};
        auto state = capture.checkpoint();
        if (!state) {
            return unavailable("party_combat", "party/combat checkpoint refused current native "
                                               "state, resources or cross-owner references");
        }
        OpponentCombatCheckpoint adapter{m_scene.m_opponents,
                                         m_combatResources->resources().sharedEffects};
        auto adapterState = SimulationCheckpointObserver::adapterValues(adapter, state->effects);
        if (!adapterState) {
            return unavailable("party_combat",
                               "represented opponent adapter values refused current native "
                               "state, resources or cross-owner references");
        }
        CombatStatePacket::State values{std::move(*state), std::move(*adapterState)};
        if (!values.valid()) {
            return unavailable("party_combat",
                               "captured combat/adapter cross-owner validation failed");
        }
        const auto body = CombatStatePacket::canonicalDigest(values.combat, values.adapter, true);
        if (!body) {
            return unavailable("party_combat",
                               "canonical combat field walker refused its record or byte bounds");
        }
        // Local composite schema 2 adds the actual native cloud/Death follow-up
        // values. They are not part of the replay adapter's restore contract.
        CanonicalDigestSink digest;
        digest.push_back(2);
        digest.append(*body);
        digest.append(m_scene.opponentFollowupDigest());
        return observed("party_combat", digest.finish(), {});
    };
    result.subsystems.push_back(combat());

    if (m_scene.m_open) {
        result.subsystems.push_back(captured<GeneratorCheckpoint>(
            "opponent_generators", m_scene.m_opponents.generators().checkpoint()));
        result.subsystems.push_back(captured<EnemyPoolCheckpoint>(
            "opponent_swarm", m_scene.m_opponents.enemies().checkpoint()));
        const auto resources = OpponentResources::missiles(m_scene.m_opponents.enemies());
        result.subsystems.push_back(captured<EnemyMissilesCheckpoint>(
            "opponent_missiles", m_scene.m_opponents.missiles().checkpoint(resources)));
        result.subsystems.push_back(captured<PlacedEnemyCheckpoint>(
            "opponent_placements", m_scene.m_opponents.placements().checkpoint()));
    } else {
        for (const auto name : {"opponent_generators", "opponent_swarm", "opponent_missiles",
                                "opponent_placements"}) {
            result.subsystems.push_back(unavailable(name, "scene is closed"));
        }
    }

    const auto stage = [&]() -> Subsystem {
        if (!m_scene.m_open || m_scene.m_world == nullptr) {
            return unavailable("stage", "scene is closed or has no world");
        }
        if (!m_combatWorld || !m_combatFixtures || !m_combatCamera || !m_combatPortals ||
            !m_combatTransporters || !m_combatStageCatalog.valid()) {
            return unavailable("stage", "load-time world/fixture/camera/portal/transporter "
                                        "checkpoint owners or trusted stage catalog are not bound");
        }
        if (!m_combatWorld->binds(*m_scene.m_world) ||
            !m_combatFixtures->binds(m_scene.m_fixtures) ||
            !m_combatCamera->binds(m_scene.m_camera, *m_scene.m_world) ||
            !m_combatPortals->binds(m_scene.m_portals) ||
            !m_combatTransporters->binds(m_scene.m_transporters)) {
            return unavailable("stage", "stage checkpoint owners no longer match live bindings");
        }
        auto world = m_combatWorld->checkpoint();
        auto fixtures = m_combatFixtures->checkpoint();
        auto camera = m_combatCamera->checkpoint();
        auto portals = m_combatPortals->checkpoint();
        auto transporters = m_combatTransporters->checkpoint();
        std::string failures;
        const auto failed = [&](bool available, std::string_view name) {
            if (!available) {
                if (!failures.empty()) {
                    failures += "; ";
                }
                failures += name;
                failures += " checkpoint refused current native state or resources";
            }
        };
        failed(world.has_value(), "world");
        failed(fixtures.has_value(), "fixtures");
        failed(camera.has_value(), "camera");
        failed(portals.has_value(), "portals");
        failed(transporters.has_value(), "transporters");
        if (!failures.empty()) {
            return unavailable("stage", std::move(failures));
        }
        StageStatePacket::State values{std::move(*world), std::move(*fixtures), std::move(*camera),
                                       std::move(*portals), std::move(*transporters)};
        if (!values.world.valid() || !values.fixtures.valid() || !values.camera->valid() ||
            values.camera->level != values.world.level || !values.portals->valid() ||
            !values.transporters->valid()) {
            return unavailable("stage", "captured stage validation failed");
        }
        const auto body = StageStatePacket::canonicalDigest(values, m_combatStageCatalog, true);
        if (!body) {
            return unavailable("stage", "canonical stage field walker refused catalog identity, "
                                        "record or decoded-byte bounds");
        }
        // Geometry is catalog-referenced in the canonical body. Its trusted
        // fingerprint is part of this diagnostic, never merely an external label.
        CanonicalDigestSink digest;
        digest.append(m_combatStageCatalog.fingerprint());
        digest.append(*body);
        return observed("stage", digest.finish(), {});
    };
    result.subsystems.push_back(stage());

    result.subsystems.push_back(observed("scene_control", m_scene.simulationControlDigest(), {}));
    return result;
}

} // namespace gdl::game
