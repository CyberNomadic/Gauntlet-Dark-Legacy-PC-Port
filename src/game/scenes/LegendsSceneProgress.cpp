#include <utility>

#include "game/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

void LegendsSceneAdapter::bindN64Gate(RenderDevice& device, formats::LegendsN64Gate definition,
                                      usize idleFigure) {
    definition.position = m_fixtures[idleFigure].position();
    ItemInstance placement;
    placement.position = definition.position;
    placement.rotation.y = definition.yaw;
    const usize opening = m_fixtures.size();
    place(device, formats::legendsN64TreeName("TRAP", definition.openingSequence), placement,
          definition.scale);
    if (m_fixtures.size() == opening) {
        m_warnings.push_back("Native locked door opening artwork is unavailable: " +
                             definition.openingSequence);
        return;
    }
    m_hiddenFixtures[idleFigure] = (definition.placementFlags & 8U) != 0;
    m_hiddenFixtures[opening] = true;
    m_gates.push_back({LegendsGate{std::move(definition)}, idleFigure, opening});
}

void LegendsSceneAdapter::touchGates(PlayerActor& actor) {
    const LegendsPortalVisitor visitor{static_cast<u8>(actor.player()), actor.position(), true,
                                       actor.radius(), actor.height()};
    for (auto& gate : m_gates) {
        auto& keys = actor.save().progress().inventory.keys;
        if (gate.state.touch(visitor, keys)) {
            if (gate.state.definition().sound != 0) {
                m_soundEvents.push_back(
                    {gate.state.definition().sound, gate.state.definition().position});
            }
            m_hiddenFixtures[gate.idleFigure] = true;
            m_hiddenFixtures[gate.openingFigure] =
                (gate.state.definition().placementFlags & 8U) != 0;
            m_fixtures[gate.openingFigure].play(0, false);
            m_fixtures[gate.openingFigure].capturePresentation();
        }
    }
}

void LegendsSceneAdapter::updateGates() {
    for (auto& gate : m_gates) {
        if (gate.state.activateInitially()) {
            if (gate.state.definition().sound != 0) {
                m_soundEvents.push_back(
                    {gate.state.definition().sound, gate.state.definition().position});
            }
            m_hiddenFixtures[gate.idleFigure] = true;
            m_hiddenFixtures[gate.openingFigure] =
                (gate.state.definition().placementFlags & 8U) != 0;
            m_fixtures[gate.openingFigure].play(0, false);
            m_fixtures[gate.openingFigure].capturePresentation();
        }
        if (gate.state.state() == LegendsGate::State::Opening &&
            m_fixtures[gate.openingFigure].finished()) {
            gate.state.finishOpening();
        }
    }
}

void LegendsSceneAdapter::bindN64Obelisk(RenderDevice& device,
                                         const formats::LegendsN64Obelisk& definition,
                                         usize idleFigure) {
    auto pickup = legendsN64ObeliskPickup(definition, nullptr, m_playerCount);
    if (!pickup) {
        return;
    }
    pickup->position = m_fixtures[idleFigure].position();
    ItemInstance placement;
    placement.position = pickup->position;
    placement.rotation.y = definition.yaw;
    const usize spent = m_fixtures.size();
    place(device, formats::legendsN64TreeName("TRAP", definition.spentSequence), placement,
          definition.scale);
    if (m_fixtures.size() == spent) {
        m_warnings.push_back("Native obelisk spent artwork is unavailable: " +
                             definition.spentSequence);
        return;
    }
    m_hiddenFixtures[spent] = true;
    m_obelisks.push_back({m_pickups.size(), idleFigure, spent, false, definition.sound});
    m_pickupFigures.push_back(idleFigure);
    m_pickups.push_back(std::move(*pickup));
}

void LegendsSceneAdapter::updateObelisks() {
    for (auto& obelisk : m_obelisks) {
        const auto& pickup = m_pickups[obelisk.pickup];
        if (!pickup.collected || obelisk.spent) {
            continue;
        }
        obelisk.spent = true;
        if (m_progressInitialized && obelisk.sound != 0) {
            m_soundEvents.push_back({obelisk.sound, pickup.position});
        }
        m_hiddenFixtures[obelisk.idleFigure] = true;
        m_hiddenFixtures[obelisk.spentFigure] = false;
        auto& figure = m_fixtures[obelisk.spentFigure];
        const auto* sequence = figure.sequenceInfo(0);
        figure.play(0, sequence && sequence->repeats);
        figure.capturePresentation();
    }
}

void LegendsSceneAdapter::synchronizeProgress(std::span<const CharacterSave* const> party) {
    LegendsProgress combined;
    for (const auto* save : party) {
        if (!save || save->character < 0 || save->character >= kClassCount) {
            continue;
        }
        const auto& progress = save->progress().legendsProgress(m_platform);
        combined.runestones |= progress.runestones;
        combined.shards |= progress.shards;
        combined.obelisks |= progress.obelisks;
        combined.legendItems |= progress.legendItems;
        // The native gate accepts each earlier stage from ANY party member.
        // Keep only the verified policy roster in this temporary union, so four
        // full save histories cannot overflow the bounded per-character record.
        for (const auto& stage : m_stageUnlocks.stages()) {
            if (progress.hasCompleted(stage.map)) {
                combined.recordCompleted(stage.map);
            }
        }
    }
    // Native e00694e4 initializes an obelisk as spent if any active character
    // owns its bit. Later contacts award the whole party in one transaction.
    if (!m_progressInitialized) {
        for (auto& obelisk : m_obelisks) {
            auto& pickup = m_pickups[obelisk.pickup];
            if (pickup.award && combined.has(pickup.award->kind, pickup.award->index)) {
                pickup.collected = true;
            }
        }
    }
    m_partyProgress = std::move(combined);
    if (m_platform == LegendsPlatform::N64 && m_gatedPortals) {
        for (usize i = 0; i < m_portals.size(); ++i) {
            auto& portal = m_portalPresentation[i];
            const bool enabled = m_stageUnlocks.allows(m_portals[i].map, m_partyProgress);
            if (portal.enabled != enabled) {
                portal.enabled = enabled;
                portal.motion.reset();
                selectPortal(i);
            }
        }
    }
    updateObelisks();
    updateTowerProgress(!m_progressInitialized);
    m_progressInitialized = true;
}

void LegendsSceneAdapter::updateTowerProgress(bool initial) {
    const auto progress = LegendsTowerProgress::of(m_partyProgress);
    for (auto& door : m_towerDoors) {
        const u8 required = door.definition.requiredObelisks;
        if (required == 0 || (m_partyProgress.obelisks & required) != required ||
            door.motion.state() != LegendsTowerDoorMotion::State::Closed) {
            continue;
        }
        if (initial && door.openFigure != door.openingFigure) {
            door.motion = LegendsTowerDoorMotion{true};
            m_hiddenFixtures[door.openingFigure] = true;
            m_hiddenFixtures[door.openFigure] = false;
        } else if (door.motion.unlock()) {
            m_heldFixtures[door.openingFigure] = false;
            m_fixtures[door.openingFigure].play(0, false);
            m_fixtures[door.openingFigure].capturePresentation();
            if (!initial) {
                // Tower overlay e0865c98 case7 releases the door's held
                // animation and calls e007a13c(0x10) exactly once.
                m_soundEvents.push_back({0x10, door.definition.triggerPosition});
            }
        }
    }
    for (usize i = 0; i < m_towerRunestones.size(); ++i) {
        if (m_towerRunestones[i]) {
            m_hiddenFixtures[*m_towerRunestones[i]] = (progress.runestones & (1U << i)) == 0;
        }
    }
    for (usize i = 0; i < m_towerShards.size(); ++i) {
        if (m_towerShards[i]) {
            m_hiddenFixtures[*m_towerShards[i]] = (progress.shards & (1U << i)) == 0;
            m_fixtureOpacity[*m_towerShards[i]] = progress.shardOpacity();
        }
    }
    if (m_towerShardPortal) {
        m_hiddenFixtures[*m_towerShardPortal] = !progress.cathedralBeam;
    }
    if (m_cathedralBeam) {
        m_hiddenWorldObjects[*m_cathedralBeam] = !progress.cathedralBeam;
    }
}

} // namespace gdl::game
