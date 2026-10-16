#pragma once

#include <memory>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/world/TextureAnimator.h"

namespace gdl::game {

/** Cropped hourglass sand, with a free-running falling-sand sprite. */
class ChallengeHud {
    struct Simulation {
        f32 frames = 0;
    };

public:
    class LocalSnapshot {
        friend class ChallengeHud;
        Simulation state;
        const ChallengeHud* owner = nullptr;
        std::weak_ptr<const u8> binding;
        TextureAnimator::Clock falling;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    struct Look {
        f32 elapsed = 1;
        s32 fallingFrame = -1; ///< -1 hides the falling grains without changing the fill.
    };
    struct Sand {
        Rect upper;
        Rect upperUv;
        Rect lower;
        Rect lowerUv;
    };
    bool bind(RenderDevice& device, ItemArchive& archive);
    void clear();
    void step(f32 seconds);
    void draw(Canvas& canvas, f32 remaining, f32 duration, bool running) const;
    Look look(f32 remaining, f32 duration, bool running) const;
    bool accepts(const Look& look) const;
    void draw(Canvas& canvas, const Look& look) const;
    static Sand sand(f32 remaining, f32 duration);

private:
    bool appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const;
    static Sand sandAt(f32 elapsed);
    const Texture* m_frame = nullptr;
    const Texture* m_sand = nullptr;
    TextureAnimator m_falling;
    Simulation m_sim;
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
};

} // namespace gdl::game
