#pragma once

#include <memory>
#include <random>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldLighting.h"

#include "game/level/FallingPiece.h"

namespace gdl {
class NativeStateDigest;
}

namespace gdl::game {
/** The altar's loose masonry, jolted twice then scattered by Skorne's entrance.
 * Borrows level meshes/textures; clear before their archives are released. */
class SkorneArena {
private:
    struct Simulation {
        std::mt19937 random{0x534b4f52U};
        s32 phase = 0;
        f32 remainder = 0;
        f32 updateSeconds = 0;
        bool operator==(const Simulation&) const = default;
        template <class Digest>
        friend void visitNativeState(Digest& digest, const Simulation& value) {
            digest.template fields<4>(value);
        }
    };

public:
    class LocalSnapshot {
        friend class SkorneArena;
        Simulation m_state;
        std::vector<FallingPiece> m_motions;
        std::weak_ptr<const u8> m_binding;
        const SkorneArena* m_owner = nullptr;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

    void bind(RenderDevice& device, const WorldLayout& layout, ModelSet& models,
              TextureSet& textures);
    void clear();
    void cue(const Vec3& boss);
    void update(f32 seconds);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              f32 presentationAlpha = -1.0f) const;
    usize size() const { return m_rocks.size(); }
    s32 phase() const { return m_sim.phase; }
    Vec3 position(usize index) const { return m_rocks.at(index).motion.position; }
    bool visible(usize index) const { return m_rocks.at(index).motion.visible; }

private:
    bool appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const;
    struct Rock {
        TreeModel model;
        FallingPiece motion;
    };
    std::vector<Rock> m_rocks;
    Simulation m_sim;
    f32 m_bottom = 0;
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
};
} // namespace gdl::game
