#include "engine/core/Types.h"

#include "game/players/CursorAim.h"
#include "game/screens/PlayScene.h"

namespace gdl::game {
std::optional<Vec3> PlayScene::cursorAim(Vec2 cursor, const Vec3& position) const {
    return m_world != nullptr && m_presentedClip
               ? cursorAimDirection(cursor, *m_presentedClip, position)
               : std::nullopt;
}

bool PlayScene::scriptedCamera() const {
    return m_sim.switchCutscene.showing() || m_arrival.camera().active() ||
           m_sim.welcome.camera().has_value() || m_promotion.active() || relicCeremonyOn();
}

bool PlayScene::cameraContinuous() const {
    return m_previousCamera.has_value() && !scriptedCamera() &&
           m_previousBossCamera == bossCameraOn();
}

WorldCamera PlayScene::viewCamera() const {
    if (const auto& camera = m_sim.switchCutscene.camera();
        m_sim.switchCutscene.showing() && camera.has_value()) {
        return *camera;
    }
    if (m_arrival.camera().active()) {
        return m_arrival.camera().camera();
    }
    if (const auto cut = m_sim.welcome.camera()) {
        return *cut;
    }
    if (const auto& promotion = m_promotion.camera();
        promotion.has_value() && !spawning() && m_promotion.active()) {
        return *promotion;
    }
    if (const auto& ceremony = m_towerRelics.camera(); ceremony.has_value() && relicCeremonyOn()) {
        return *ceremony;
    }
    return bossCameraOn() ? m_sim.shake.apply(m_sim.bossCamera.camera(), m_sim.bossCamera.attention())
                          : m_sim.shake.apply(m_camera.camera(), m_camera.attention());
}

/** A boss level with a boss camera record frames the fight with it while the boss stands. */
bool PlayScene::bossCameraOn() const {
    const LevelInfo* level = m_world != nullptr ? m_world->level() : nullptr;
    return level != nullptr && level->bossCamera.has_value() &&
           (m_opponents.bosses().present() || m_bossSequence.fallen());
}

/** The boss as the camera sees it; once it has fallen, the wizard in its place. */
BossCameraSubject PlayScene::bossSubject() const {
    BossCameraSubject subject;
    if (m_bossSequence.victory().state().running()) {
        return m_bossSequence.victorySubject();
    }
    if (!m_opponents.bosses().present() && m_bossSequence.fallen()) {
        return m_bossSequence.restSubject();
    }
    if (const Vec3* at = m_opponents.bosses().position(); at != nullptr) {
        subject.position = *at;
    }
    subject.facing = m_opponents.bosses().facing();
    subject.radius = m_opponents.bosses().radius();
    subject.height = m_opponents.bosses().height();
    subject.attentionOffset = m_opponents.bosses().cameraOffset();
    subject.baseAttention = m_opponents.bosses().cameraBase();
    subject.awake = m_opponents.bosses().view().awake;
    return subject;
}

/** Canonical gameplay projection for camera fitting, visibility, party limits and
 * transporter selection. Local FOV/aspect belong only to render() and its
 * presented cursor projection; they must not change the next native update. */
CameraView PlayScene::cameraView() {
    return {};
}

} // namespace gdl::game
