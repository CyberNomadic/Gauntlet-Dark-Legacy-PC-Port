#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "engine/assets/AssetScope.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldCollision.h"

#include "game/world/TowerCamera.h"

namespace gdl::game {

/** Paired TRANS items move individual players within a level, not through its exits. */
class LevelTransporters {
public:
    static constexpr s32 kItemType = 11;
    struct Contact {
        s32 instance = -1;
        s32 id = 0;
        s32 destinationId = 0;
        std::optional<usize> destination;
        Vec3 position{0.0f};
        f32 radius = 0.0f;
        f32 height = 0.0f;
        Mat4 transform{1.0f};
        s32 support = -1;        ///< the world object it stands on, when that one moves
        Mat4 supportLocal{1.0f}; ///< its art, in the support's own space
        Vec3 anchorLocal{0.0f};  ///< its contact and landing point, likewise
        void sync(const WorldCollision& collision);
        bool touches(const Vec3& feet, f32 bodyRadius, f32 bodyHeight) const;
    };
    /** The same load-time support binding drives native pads and transactional
     * travel-boundary checks; sync the value at the native floor-carry phase. */
    static std::vector<Contact> contacts(const WorldLayout& layout, s32 players,
                                         const WorldCollision* collision = nullptr);
    struct Pad : Contact {
        mutable TreeModel model;
        TreePose pose;
        AnimationPlayer animation;
        f32 previousFrame = 0;
        u64 previousGeneration = 0;
    };

    /** The scope's archives must outlive the pads; the first holding TRANS supplies them. A
     * pad on a moving floor is carried by it (AddItemSub parents it to the floor's node). */
    void bind(RenderDevice& device, const WorldLayout& layout, const AssetScope& scope, s32 players,
              const WorldCollision* collision = nullptr);
    void clear();
    /** Carries the pads with their moving floors: art, contact and landing point alike. */
    void syncFloors(const WorldCollision& collision);
    void animate(f32 seconds);
    void capturePresentation() {
        m_presentationAdvanced = false;
        m_textures.advance(0);
    }
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              f32 renderAlpha = -1.0f) const;
    usize size() const { return m_pads.size(); }
    const Pad& pad(usize index) const { return m_pads[index]; }
    std::optional<usize> contact(const Vec3& feet, f32 radius, f32 height) const;
    /** Reject missing links, offscreen destinations and landings with no floor. */
    std::optional<Vec3> landing(usize source, f32 radius, const WorldCollision& collision,
                                const WorldCamera& camera, const CameraView& view) const;
    static bool visible(const Vec3& position, f32 radius, const WorldCamera& camera,
                        const CameraView& view);
    static std::string_view soundForRealm(s32 realm);

private:
    std::vector<Pad> m_pads;
    const TreeInfo* m_tree = nullptr;
    TextureAnimator m_textures;
    bool m_presentationAdvanced = false;
};

} // namespace gdl::game
