#pragma once

#include "game/netplay/PartyBootstrap.h"
#include "game/screens/ReplicaView.h"
#include "game/screens/TurboEffectResources.h"

namespace gdl::game {
class LocalTurboEffectPrediction;
class OpponentPrediction;
class CombatPrediction;
/** Owns a client's native level artwork and its immutable resource catalogs.
 * There is no PlayScene, gameplay update, sound player or save storage. Resolve
 * the level through the local catalog before open(), never from a packet path.
 * Failed loading leaves the previous stage intact. The device must outlive us. */
class ReplicaStage {
public:
    ReplicaStage();
    ~ReplicaStage();
    ReplicaStage(const ReplicaStage&) = delete;
    ReplicaStage& operator=(const ReplicaStage&) = delete;
    ReplicaStage(ReplicaStage&&) = delete;
    ReplicaStage& operator=(ReplicaStage&&) = delete;
    bool open(RenderDevice& device, const std::filesystem::path& root, const LevelRef& level,
              const MatchContext& context, const PartyBootstrap::Party& party,
              const StringTable* strings = nullptr, f32 difficultyGain = 1);
    bool resume(const MatchContext& context);
    void clear();
    bool show(const CombatSnapshot& snapshot, const EnemyPresentation* presentation = nullptr);
    void draw(RenderDevice& device, const Mat4& frameProjection, f32 width, f32 height,
              f32 textureFrame, const GameConfig& video = {},
              const LocalTurboEffectPrediction* effects = nullptr, f32 effectBlend = 1);
    const ReplicaView* view() const;
    const LevelWorld* world() const;
    const ProjectileResources* projectiles() const;
    /** Trusted load-time class identity and move rows, owned until the next open/clear. */
    s32 character(u8 seat) const;
    const ClassStats* classStats(u8 seat) const;
    const TurboEffectResources* turboEffects(u8 seat) const;
    const ItemArchive* weapons() const;
    OpponentPrediction* opponentPrediction();
    CombatPrediction* combatPrediction();

private:
    struct Assets;
    std::unique_ptr<Assets> m_assets;
    u64 m_epoch = 0;
};
} // namespace gdl::game
