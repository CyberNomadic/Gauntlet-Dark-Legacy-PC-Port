#pragma once

#include "game/audio/LegendsSounds.h"
#include "game/bosses/LegendsBosses.h"
#include "game/level/ImportedLevel.h"
#include "game/level/LegendsBlast.h"
#include "game/scenes/LegendsPreview.h"

namespace gdl::game {

/** Native map resources hosted by PlayScene, not a second player implementation. */
class LegendsLevel final : public ImportedLevel {
public:
    LegendsLevel(RenderDevice& device, const formats::LegendsMap& map,
                 std::unique_ptr<LegendsSceneAdapter> adapter,
                 const std::filesystem::path& directory,
                 const std::filesystem::path& characterRoot);
    void capturePresentation() override;
    void update(f32 seconds, bool timeStopped) override;
    void bindCollision(WorldCollision& collision) const override;
    void syncCollision(WorldCollision& collision) const override;
    void draw(RenderDevice& device, const Mat4& clip, const WorldCamera& view, TreeModel::Pass pass,
              f32 alpha) const override;
    void resetCamera(std::span<const CameraSubject> subjects) override;
    void followCamera(std::span<const CameraSubject> subjects, f32 seconds) override;
    std::optional<WorldCamera> camera(f32 alpha = -1) const override;
    std::optional<Vec3> cameraAttention() const override;
    Vec3 resolveMovement(const Vec3& from, const Vec3& to, f32 radius, f32 height) const override;
    void updateParty(std::span<PlayerActor*> players, f32 seconds, bool activatePowerups) override;
    void initializeParty(std::span<const PartyMember> party);
    /** Only the app's completed native departure calls this; console jumps do not. */
    void completeTravel(std::span<PartyMember> party) const;
    std::vector<ImportedPickup> takePickups() override;
    std::vector<ImportedTarget> targets() const override;
    bool strike(s32 id, f32 power, u32 flags, s32 player) override;
    std::vector<Obstacle> obstacles() const override;
    bool travelRequested() const override { return m_destination.has_value(); }
    std::optional<ImportedDeparture> departure(f32 alpha = -1) const override;
    std::span<const ImportedGenerator> importedGenerators() const override;
    std::span<const ImportedActor> importedActors() const override {
        return m_world.adapter()->actors();
    }
    ItemArchive* importedActorArchive(s32 kind) const override {
        return m_world.adapter()->actorArchive(kind);
    }
    const ImportedBoss* importedBoss() const override { return m_boss ? &m_boss->combat : nullptr; }
    void bossDefeated(const Vec3& position, std::span<PlayerActor*> players) override;
    std::vector<Mat4> arenaAnchors() const override { return adapter().arenaAnchors(); }
    std::vector<CombatArenaTarget> arenaTargets() const override {
        return adapter().arenaTargets();
    }
    void activateArena(const CombatArenaActivation& activation) override {
        m_world.adapter()->scheduleArenaActivation(activation.index, activation.delay);
    }
    bool arenaBlocksSegment(const Vec3& from, const Vec3& to, f32 radius) const override {
        return adapter().arenaBlocksSegment(from, to, radius);
    }
    std::vector<MissileStop> arenaMissileStops() const override {
        return adapter().arenaMissileStops();
    }
    void updateGenerators(std::span<const ImportedGeneratorState> states) override;
    void watch(const ViewVolume& view) override { m_world.adapter()->setTrapView(view); }
    std::vector<ImportedHazardHit> takeHazardHits() override;
    void affectArea(const ImportedAreaDamage& apply) override;
    void playSounds(SoundPlayer& output, const Vec3& listener) override;
    void pauseSounds() override;
    void stopSounds() override;
    const std::optional<std::string>& destination() const { return m_destination; }
    const std::filesystem::path& directory() const { return m_directory; }
    LegendsPlatform platform() const { return m_world.adapter()->platform(); }
    const LegendsSceneAdapter& adapter() const { return *m_world.adapter(); }

private:
    LegendsPreview m_world;
    std::filesystem::path m_directory;
    std::optional<LegendsCamera> m_camera;
    f32 m_cameraTickSeconds = 1.0f / 60.0f;
    std::optional<std::string> m_destination;
    f64 m_departureSeconds = 0;
    f64 m_previousDepartureSeconds = 0;
    std::unique_ptr<Texture> m_departureSkin;
    ClassDataSet m_classes;
    std::vector<ImportedPickup> m_pickups;
    LegendsSounds m_sounds;
    SoundPlayer* m_soundOutput = nullptr;
    std::vector<SoundHandle> m_soundHandles;
    struct MotionSound {
        u32 id = 0;
        SoundHandle handle = kNoSound;
    };
    std::vector<MotionSound> m_motionSounds;
    std::vector<LegendsBlast> m_blasts;
    std::vector<ImportedHazardHit> m_hazardHits;
    std::vector<LegendsPortalVisitor> m_visitors;
    f32 m_trapDamageScale = 1;
    std::optional<LegendsBossEncounter> m_boss;
    bool m_bossDefeated = false;
    f64 m_bossDepartureSeconds = 0;
    Vec3 m_playerStart{0};
    mutable const WorldCollision* m_collision = nullptr; ///< borrowed from the hosting LevelWorld
};

} // namespace gdl::game
