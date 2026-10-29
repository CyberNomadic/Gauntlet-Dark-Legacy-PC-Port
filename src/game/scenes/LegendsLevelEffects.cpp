#include <algorithm>
#include <utility>

#include "game/combat/Damage.h"
#include "game/combat/DamageTypes.h"
#include "game/party/PlayerMissiles.h"
#include "game/scenes/LegendsLevel.h"

namespace gdl::game {

std::vector<LegendsFixtureEvent> LegendsSceneAdapter::takeFixtureEvents() {
    return std::exchange(m_fixtureEvents, {});
}

std::vector<LegendsSoundEvent> LegendsSceneAdapter::takeSounds() {
    return std::exchange(m_soundEvents, {});
}

void LegendsLevel::affectArea(const ImportedAreaDamage& apply) {
    // New blasts are authored by fixture strikes, including chains caused by a
    // prior blast. Their clocks/hit lists remain native; health is always GDL's.
    const auto admit = [&] {
        for (auto& event : m_world.adapter()->takeFixtureEvents()) {
            m_blasts.emplace_back(std::move(event), m_trapDamageScale);
        }
    };
    admit();
    for (usize i = 0; i < m_blasts.size(); ++i) {
        auto& blast = m_blasts[i];
        const auto area = blast.step(0);
        const auto& event = blast.event();
        const bool poison = event.kind == LegendsFixtureEvent::Kind::Poison;
        if (area && area->radius > 0) {
            for (const auto& visitor : m_visitors) {
                if (const auto damage = blast.touch(visitor)) {
                    const Vec3 direction = visitor.position - event.position;
                    m_hazardHits.push_back(
                        {visitor.player,
                         *damage,
                         false,
                         poison,
                         {Damage::kLow | (poison ? 0U : damage::kFire | PlayerImpact::kKnockDown),
                          direction}});
                }
            }
            if (apply) {
                // Native flags use a different layout. Do not forward them to
                // GDL's fire/explosion/knockdown/poison damage dispatcher.
                apply(area->position, area->radius, area->damage, poison ? 0x800U : 0x421U,
                      blast.reachedOpponents());
            }
            // Native PTrap/PCon thresholds use physical damage only. In
            // particular, poison can ignite an explosive (threshold 1) without
            // destroying wooden barrels (threshold 6) with its elemental part.
            for (const auto& target : targets()) {
                auto& reached = blast.reachedFixtures();
                if (!target.valid() || !target.damageable ||
                    std::ranges::find(reached, target.id) != reached.end()) {
                    continue;
                }
                const MissileTarget body{target.id, target.base, target.radius, target.height};
                if (body.touches(area->position, area->radius)) {
                    reached.push_back(target.id);
                    strike(target.id,
                           blast.physicalDamageAt(target.base + Vec3{0, target.height * .5f, 0}),
                           0x400U, event.player);
                }
            }
        }
        admit(); // May reallocate m_blasts: no reference above survives this point.
    }
}

std::vector<ImportedHazardHit> LegendsLevel::takeHazardHits() {
    for (const auto& hit : m_world.adapter()->takeTrapHits()) {
        m_hazardHits.push_back({hit.player, hit.damage, hit.pierces, false, hit.impact});
    }
    return std::exchange(m_hazardHits, {});
}

void LegendsLevel::playSounds(SoundPlayer& output, const Vec3& listener) {
    if (m_soundOutput && m_soundOutput != &output) {
        stopSounds();
    }
    m_soundOutput = &output;
    const auto loops = m_world.adapter()->soundLoops();
    const auto audible = [&](const Vec3& position) {
        return std::clamp((70.0f - glm::distance(listener, position)) / 50.0f, 0.0f, 1.0f);
    };
    std::erase_if(m_motionSounds, [&](const MotionSound& sound) {
        const auto desired = std::ranges::find(loops, sound.id, &LegendsSoundLoop::id);
        const bool playing = output.isPlaying(sound.handle);
        if (!playing || desired == loops.end() || audible(desired->position) <= 0) {
            if (playing) {
                output.stop(sound.handle);
            }
            return true;
        }
        return false;
    });
    for (const auto& desired : loops) {
        const f32 volume = audible(desired.position);
        if (volume <= 0) {
            continue;
        }
        auto active = std::ranges::find(m_motionSounds, desired.id, &MotionSound::id);
        if (active == m_motionSounds.end()) {
            const auto handle = m_sounds.play(output, desired.token, volume, true);
            if (handle != kNoSound) {
                m_motionSounds.push_back({desired.id, handle});
            }
        } else {
            output.setVolume(active->handle, volume);
        }
    }
    std::erase_if(m_soundHandles, [&](SoundHandle handle) { return !output.isPlaying(handle); });
    for (const auto& cue : m_world.adapter()->takeSounds()) {
        // Match the common scene's near/far audibility without changing the
        // authored native macro gain, envelopes or pitch.
        const f32 volume = audible(cue.position);
        if (volume > 0) {
            const auto handle = m_sounds.play(output, cue.token, volume);
            if (handle != kNoSound) {
                m_soundHandles.push_back(handle);
            }
        }
    }
}

void LegendsLevel::pauseSounds() {
    if (m_soundOutput) {
        for (const auto& sound : m_motionSounds) {
            m_soundOutput->stop(sound.handle);
        }
    }
    m_motionSounds.clear();
}

void LegendsLevel::stopSounds() {
    pauseSounds();
    if (m_soundOutput) {
        for (const auto handle : m_soundHandles) {
            m_soundOutput->stop(handle);
        }
    }
    m_soundHandles.clear();
    m_soundOutput = nullptr;
}

} // namespace gdl::game
