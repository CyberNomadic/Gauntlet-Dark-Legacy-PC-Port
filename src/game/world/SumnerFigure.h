#pragma once

#include <array>
#include <memory>
#include <string_view>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldLighting.h"

namespace gdl::game {

/**
 * Sumner the good wizard as he stands in his tower: the level's item archive's figure at the
 * lookout the level marks for him, idling through his stance, his reading and his thinking
 * one after the other the way the original cycles them, and gesturing at once when asked.
 */
class SumnerFigure {
    struct Simulation {
        s32 index = 0;
        bool cutIn = false;
        AnimationPlayer player;
        TreePose pose;
        f32 previousFrame = 0;
        u64 previousGeneration = 0;
        bool presentationAdvanced = false;
    };

public:
    class LocalSnapshot {
        friend class SumnerFigure;
        Simulation state;
        const SumnerFigure* owner = nullptr;
        std::weak_ptr<const u8> binding;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    static constexpr std::string_view kTree = "GWIZ";
    /** His sequences in the original's order; the first three are the idle cycle. */
    static constexpr std::array<std::string_view, 7> kSequences{
        "READY", "READING", "THINKING", "WELCOME", "GOAWAY", "GESTLEFT", "GESTRIGHT"};
    static constexpr s32 kWelcomeIndex = 3; ///< greeting a player who steps up to him
    static constexpr s32 kGoAwayIndex = 4;  ///< seeing them off once they are done
    static constexpr s32 kGestureIndex = 6; ///< the sweep towards the crystals
    static constexpr u32 kLookout = 0;      ///< the event marker parameter naming his spot

    /** Builds the figure from `items`, which must outlive it, and stands it at the layout's
     * lookout facing back along the marker's heading; false (with a warning) when the
     * archive, the tree or the marker is missing. */
    bool load(RenderDevice& device, ItemArchive& items, const WorldLayout& layout);
    void clear();
    bool loaded() const { return m_tree != nullptr; }

    /** Cuts to the sweep towards the crystals; the idle cycle resumes from the stance after
     * it. */
    void gesture() { play(kGestureIndex); }
    /** Cuts to one of kSequences at once; the idle cycle resumes after it. */
    void play(s32 index);
    void update(f32 seconds);
    /** Marks fixed updates that hold his animation as well as those that advance it. */
    void capturePresentation() { m_sim.presentationAdvanced = false; }
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              f32 frameBlend = -1.0f) const;

    const Vec3& position() const { return m_position; }
    f32 yaw() const { return m_yaw; }
    /** The sequence playing, and the index the cycle asks for next. */
    u32 sequence() const { return m_sim.player.sequence(); }
    const TreeInfo* tree() const { return m_tree; }
    const AnimationPlayer& animation() const { return m_sim.player; }
    const Mat4& placement() const { return m_transform; }
    s32 index() const { return m_sim.index; }
    bool gesturing() const { return playing(kGestureIndex); }
    /** Whether the sequence of that index is the one playing. */
    bool playing(s32 index) const {
        return index >= 0 && static_cast<usize>(index) < m_sequences.size() &&
               m_sequences[static_cast<usize>(index)] >= 0 &&
               sequence() == static_cast<u32>(m_sequences[static_cast<usize>(index)]);
    }

private:
    void appendState(NativeStateDigest& digest, const Simulation& state) const;
    u32 sequenceFor(s32 index) const;

    mutable TreeModel m_model;
    const TreeInfo* m_tree = nullptr;
    std::array<s32, kSequences.size()> m_sequences{-1, -1, -1, -1, -1, -1, -1};
    Simulation m_sim;
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
    Vec3 m_position{0.0f, 0.0f, 0.0f};
    f32 m_yaw = 0.0f;
    Mat4 m_transform{1.0f};
};

} // namespace gdl::game
