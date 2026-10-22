#include "game/party/PortalDeparture.h"

#include <algorithm>
#include <exception>
#include <format>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"

namespace gdl::game {
bool PortalDeparture::appendNativeDigest(NativeStateDigest& digest) const {
    digest.string("PortalDeparture.native.1");
    digest.fields<2>(m_sim);
    return digest.good();
}

bool PortalDeparture::appendLocalDigest(NativeStateDigest& digest,
                                        const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    const auto& [state, owner, binding] = saved;
    // Skin frames/device are immutable presentation resources, lazily retained
    // across an undone first departure. They cannot affect its native timer.
    digest.string("PortalDeparture.native.1");
    digest.fields<2>(state);
    return digest.good();
}

void PortalDeparture::saveLocal(LocalSnapshot& saved) const {
    saved.m_owner = this;
    saved.m_binding = m_localBinding;
    saved.m_state = m_sim;
}

bool PortalDeparture::canRestoreLocal(const LocalSnapshot& saved) const {
    const auto binding = saved.m_binding.lock();
    return saved.m_owner == this && binding && binding == m_localBinding;
}

bool PortalDeparture::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    return true;
}

void PortalDeparture::begin(RenderDevice& device, TextureSet& weapons) {
    // Starting is gameplay state, not a new stage binding. A correction may undo
    // the first start and replay it using the same retained skin frames.
    m_sim = {.ticks = 0, .started = true};
    loadSkin(device, weapons);
}

void PortalDeparture::loadSkin(RenderDevice& device, TextureSet& weapons) {
    if (m_device == &device && m_weapons == &weapons) {
        return;
    }
    if (m_device != nullptr || m_weapons != nullptr) {
        m_localBinding = std::make_shared<const u8>(0);
    }
    m_device = &device;
    m_weapons = &weapons;
    m_frames.fill(nullptr);
    for (usize i = 0; i < m_frames.size(); ++i) {
        const auto name = i == 0 ? "DTH_LIGHT00" : std::format("DTH_LIGHT00+{}", i);
        if (const auto index = weapons.find(name)) {
            try {
                m_frames[i] = &weapons.texture(device, *index);
            } catch (const std::exception& e) {
                log::warn("Portal skin: {}", e.what());
            }
        }
    }
}

void PortalDeparture::clear() {
    m_localBinding = std::make_shared<const u8>(0);
    m_frames.fill(nullptr);
    m_device = nullptr;
    m_weapons = nullptr;
    m_sim = {};
}

void PortalDeparture::update(s32 ticks) {
    if (m_sim.started) {
        m_sim.ticks = std::min(m_sim.ticks + std::max(ticks, 0), kTicks);
    }
}

Mat4 PortalDeparture::transform(const Mat4& body) const {
    if (!m_sim.started) {
        return body;
    }
    return transformAt(body, phase());
}

Mat4 PortalDeparture::transformAt(const Mat4& body, f32 phase) {
    const f32 elapsed = std::clamp(phase, 0.0f, 1.0f) * kTicks;
    Mat4 result = portable::rotate(body, kSpinPerSecond * elapsed / 60.0f, Vec3{0, 1, 0});
    result[3] += Vec4{0, -elapsed * kSinkPerTick, 0, 0};
    return result;
}

const Texture* PortalDeparture::skin() const {
    return m_sim.started ? skinAt(phase()) : nullptr;
}

const Texture* PortalDeparture::skinAt(f32 phase) const {
    if (phase < 0 || phase >= 1) {
        return nullptr;
    }
    // Skin rate 0.4 per 30 Hz frame, independently of the texture's free-running rate.
    const auto frame = static_cast<usize>(phase * kTicks * 0.2f);
    return m_frames[std::min(frame, m_frames.size() - 1)];
}
} // namespace gdl::game
