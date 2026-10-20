#pragma once

#include <memory>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include "engine/core/Types.h"

#include "game/screens/BossVictoryPresentation.h"
#include "game/screens/LegendPresentation.h"
#include "game/screens/LevelFixtures.h"
#include "game/screens/LevelOpponents.h"
#include "game/screens/PlayerRuntime.h"
#include "game/world/LevelCatalog.h"
#include "game/world/LevelSoundscape.h"
#include "game/world/LevelWorld.h"

namespace gdl::game {

/** Coordinates legend-item impacts and boss-death rewards, coins and the wizard visit.
 * Borrows level services, never the scene. Travel and draw order remain with the caller. */
class BossSequence {
public:
    struct Resources {
        RenderDevice& device;
        LevelWorld& world;
        ItemArchive& weapons;
        TextureSet& textures;
        EffectTrees& effects;
        LevelSoundscape& audio;
        const LevelCatalog* levels = nullptr;
    };

    BossSequence() = default;
    BossSequence(const BossSequence&) = delete;
    BossSequence& operator=(const BossSequence&) = delete;
    BossSequence(BossSequence&&) = delete;
    BossSequence& operator=(BossSequence&&) = delete;
    ~BossSequence() = default;

    void bind(Resources resources);
    /** Clear before releasing borrowed audio, archives or effects. Does not rewind coin RNG. */
    void clear();
    void showLegend(const LegendEvent& event, const Bosses& bosses,
                    std::span<const PlayerRuntime> players);
    void advanceLegend(f32 seconds, Bosses& bosses, std::span<PlayerRuntime> players);
    void gatherAudio() const;
    bool hasLegacyAudio() const {
        return m_victoryHandle != kNoSound || (m_legend != nullptr && m_legend->hasLegacyAudio());
    }
    void fallen(const Vec3& where, const Bosses& bosses, std::span<PlayerRuntime> players);
    /** The death's blast takes the level's enemies, generators and arena cover with it
     * before the coins fly. */
    void spewCoins(const CombatSpew& spew, LevelOpponents& opponents, LevelFixtures& fixtures,
                   std::span<PlayerRuntime> players);
    /** True only on the update that completes the visit; the caller chooses how to travel. */
    bool advanceVictory(s32 ticks, f32 seconds, std::span<const PlayerRuntime> players,
                        const MessageTable& strings);
    const BossVictoryPresentation& victory() const { return m_victory; }
    /** Once per native scene update, including updates held by other scene phases. */
    void advanceAudio(f32 seconds) { m_sim.speech.advance(seconds); }
    const SpeechTiming& speechTiming() const { return m_sim.speech; }
    void capturePresentation() { m_victory.capturePresentation(); }
    BossCameraSubject victorySubject() const;
    /** Whether the boss has fallen this level, the wizard's visit begun or done. The boss
     * camera stays with the arena from the fall to the level's end, as BossCameraUpdate's
     * does (its active flag clears only with the next level's camera). */
    bool fallen() const { return m_victory.state().running() || m_victory.state().finished(); }
    /** Where the boss's key lies. */
    const Vec3& shardPosition() const { return m_sim.shardPosition; }
    /** The camera's subject once the wizard has gone: BossCameraUpdate looks at the
     * fallen boss's base, with the key's radius, and keeps the party in view. */
    BossCameraSubject restSubject() const;
    const Texture* frozenTexture() const {
        return m_legend != nullptr ? m_legend->frozenTexture() : nullptr;
    }
    s32 occupiedHand() const { return m_legend != nullptr ? m_legend->occupiedHand() : -1; }
    /** Player ids need not be contiguous; missing figures still supply fallback hold points. */
    static std::optional<LegendPresentation::Bearer> bearer(s32 player, s32 kind,
                                                            std::span<const PlayerRuntime> players);

private:
    struct Simulation {
        std::mt19937 coinRandom{0xC01Eu};
        SpeechTiming speech;
        SoundCueToken speechCue;
        Vec3 shardPosition{0};
        Vec3 fallenSpot{0};      ///< where the boss fell
        f32 fallenFacing = 0.0f; ///< and the way it faced
    };

public:
    class LocalSnapshot {
        friend class BossSequence;
        Simulation state;
        BossVictoryPresentation::LocalSnapshot victory;
        std::optional<LegendPresentation::LocalSnapshot> legend;
        const BossSequence* owner = nullptr;
        std::weak_ptr<const u8> binding;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

private:
    Simulation m_sim;
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
    std::optional<Resources> m_resources;
    BossVictoryPresentation m_victory;
    SoundHandle m_victoryHandle = kNoSound;
    std::vector<SoundHandle> m_victoryVoices;
    std::unique_ptr<LegendPresentation> m_legend;
};
} // namespace gdl::game
