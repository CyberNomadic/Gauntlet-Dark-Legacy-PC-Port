#pragma once

#include <array>
#include <memory>
#include <string_view>

#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl {
class NativeStateDigest;
}

namespace gdl::game {

/** The committed portal departure: held controls, lightning skin, then a sinking spin.
 * The camera follows the same downward displacement as the rendered bodies. */
class PortalDeparture {
    struct Simulation {
        s32 ticks = 0;
        bool started = false;
    };

public:
    class LocalSnapshot {
        friend class PortalDeparture;
        Simulation state;
        const PortalDeparture* owner = nullptr;
        std::weak_ptr<const u8> binding;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    static constexpr s32 kTicks = 50;
    static constexpr std::string_view kSound = "S_TUNNEL";
    static constexpr f32 kVolume = 127.0f / 255.0f;
    static constexpr f32 kSinkPerTick = 0.12f;
    static constexpr f32 kSpinPerSecond = 3.0f * glm::pi<f32>();
    void begin(RenderDevice& device, TextureSet& weapons);
    /** Preload presentation resources without starting a departure. */
    void loadSkin(RenderDevice& device, TextureSet& weapons);
    void clear();
    void update(s32 ticks);
    bool started() const { return m_sim.started; }
    bool finished() const { return m_sim.started && m_sim.ticks >= kTicks; }
    f32 phase() const { return static_cast<f32>(m_sim.ticks) / kTicks; }
    Vec3 displacement() const { return {0, -static_cast<f32>(m_sim.ticks) * kSinkPerTick, 0}; }
    Mat4 transform(const Mat4& body) const;
    const Texture* skin() const;
    /** Sample the committed departure without advancing gameplay or playing cues. */
    static Mat4 transformAt(const Mat4& body, f32 phase);
    const Texture* skinAt(f32 phase) const;

private:
    std::array<const Texture*, 10> m_frames{};
    Simulation m_sim;
    const TextureSet* m_weapons = nullptr;
    const RenderDevice* m_device = nullptr;
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
};

} // namespace gdl::game
