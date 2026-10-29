#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCamera.h"

#include "game/camera/TowerCamera.h"
#include "game/critters/CombatEvents.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/level/ImportedActors.h"
#include "game/level/ImportedBoss.h"
#include "game/level/ImportedGenerators.h"
#include "game/level/ImportedHazards.h"
#include "game/level/ImportedTargets.h"
#include "game/level/ItemFigure.h"
#include "game/players/ItemPickup.h"

namespace gdl::game {
class PlayerActor;

/** Feedback for an already completed native pickup, never a second inventory grant.
 * Player identifies the input/HUD slot, not an index into the living visitors.
 * Native campaign awards leave offer empty rather than borrowing GDL relic kinds. */
struct ImportedPickup {
    s32 player = 0;
    ItemOffer offer;
    std::string name;
    ItemTaking taking;
    std::vector<std::string> scrollPages;
};

/** Presentation about each departing player's existing feet/orientation.
 * Optional native overrides leave ordinary Dark Legacy departures unchanged. */
struct ImportedDeparture {
    f32 yaw = 0;
    f32 scale = 1;
    const Texture* skin = nullptr; ///< owned by the imported level, preloaded before play
    bool finished = false;
};

/** Native level presentation and scenery rules hosted by the ordinary gameplay scene.
 * Character controls, combat, inventory, HUD and pause remain PlayScene responsibilities.
 * Implementations retain their native archive resources for as long as LevelWorld uses them.
 * Imported levels are currently offline; this interface is not a rollback state contract. */
class ImportedLevel {
public:
    virtual ~ImportedLevel() = default;
    virtual void capturePresentation() = 0;
    virtual void update(f32 seconds, bool timeStopped) = 0;
    virtual void draw(RenderDevice& device, const Mat4& clip, const WorldCamera& camera,
                      TreeModel::Pass pass, f32 presentationAlpha) const = 0;
    /** Mark native moving surface owners once, then synchronize their tick poses
     * before the common player/enemy floor-riding phase. */
    virtual void bindCollision(WorldCollision& /*collision*/) const {}
    virtual void syncCollision(WorldCollision& /*collision*/) const {}

    virtual void resetCamera(std::span<const CameraSubject> /*subjects*/) {}
    virtual void followCamera(std::span<const CameraSubject> /*subjects*/, f32 /*seconds*/) {}
    virtual std::optional<WorldCamera> camera(f32 /*alpha*/ = -1.0f) const { return std::nullopt; }
    virtual std::optional<Vec3> cameraAttention() const { return std::nullopt; }
    virtual Vec3 resolveMovement(const Vec3& /*from*/, const Vec3& to, f32 /*radius*/,
                                 f32 /*height*/) const {
        return to;
    }
    /** Post-motion living visitors only. Grants and local transport are native
     * level rules; the common scene still owns inventory and departure effects. */
    virtual void updateParty(std::span<PlayerActor*> /*players*/, f32 /*seconds*/,
                             bool /*timeStopped*/) {}
    virtual std::vector<ImportedPickup> takePickups() { return {}; }
    /** Ordinary melee, missiles and area effects share these native fixtures.
     * Inert bodies remain physical targets but never acquire an attack. */
    virtual std::vector<ImportedTarget> targets() const { return {}; }
    virtual bool strike(s32 /*target*/, f32 /*power*/, u32 /*flags*/, s32 /*byPlayer*/) {
        return false;
    }
    virtual std::vector<Obstacle> obstacles() const { return {}; }
    virtual std::span<const ImportedGenerator> importedGenerators() const { return {}; }
    virtual std::span<const ImportedActor> importedActors() const { return {}; }
    /** Optional native body supplement, borrowed until this level unloads.
     * Movement/combat remain in the common pools, not in the asset adapter. */
    virtual ItemArchive* importedActorArchive(s32 /*kind*/) const { return nullptr; }
    virtual const ImportedBoss* importedBoss() const { return nullptr; }
    /** Native death completion, after animation/hold; never a lethal-hit grant. */
    virtual void bossDefeated(const Vec3& /*position*/, std::span<PlayerActor*> /*players*/) {}
    virtual std::vector<Mat4> arenaAnchors() const { return {}; }
    virtual std::vector<CombatArenaTarget> arenaTargets() const { return {}; }
    virtual void activateArena(const CombatArenaActivation& /*activation*/) {}
    virtual bool arenaBlocksSegment(const Vec3& /*from*/, const Vec3& /*to*/,
                                    f32 /*radius*/) const {
        return false;
    }
    virtual std::vector<MissileStop> arenaMissileStops() const { return {}; }
    virtual void updateGenerators(std::span<const ImportedGeneratorState> /*states*/) {}
    virtual void watch(const ViewVolume& /*view*/) {}
    virtual std::vector<ImportedHazardHit> takeHazardHits() { return {}; }
    virtual void affectArea(const ImportedAreaDamage& /*damage*/) {}
    /** Called outside the native tick; shared audio settings and pause apply. */
    virtual void playSounds(SoundPlayer& /*player*/, const Vec3& /*listener*/) {}
    virtual void pauseSounds() {}
    virtual void stopSounds() {}
    virtual bool travelRequested() const { return false; }
    virtual std::optional<ImportedDeparture> departure(f32 /*alpha*/ = -1.0f) const {
        return std::nullopt;
    }
};

} // namespace gdl::game
