#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldLighting.h"

#include "game/level/LevelCatalog.h"
#include "game/level/LevelItemArchives.h"

namespace gdl::game {

/**
 * World objects stood where the developer console asks, out of any level's archive: a level's
 * layout, models, textures and borrowed item textures are loaded the first time one of its
 * objects is wanted and kept, and each object is bound with its descendants as a figure of its
 * own, drawn with the level in play's lighting. The level in play's own objects are drawn
 * from its batches and never move; these are copies set down beside them.
 */
class SpawnedObjects {
    struct Spawn {
        std::string level;
        std::string name;
        Mat4 transform{1.0f};
        bool operator==(const Spawn&) const = default;
        template <class Digest> friend void visitNativeState(Digest& digest, const Spawn& value) {
            digest.template fields<3>(value);
        }
    };

public:
    static constexpr usize kMaxObjects = 256;
    /** Only the active roster rewinds. Loaded geometry stays bound for the stage. */
    class LocalSnapshot {
        friend class SpawnedObjects;
        std::vector<Spawn> m_spawned;
        const SpawnedObjects* m_owner = nullptr;
        std::weak_ptr<const u8> m_binding;
    };

    SpawnedObjects() = default;
    ~SpawnedObjects() = default;
    GDL_NON_COPYABLE_NON_MOVABLE(SpawnedObjects);

    /** Stands `name` of `level` at `transform`; false when the level is not unpacked or has
     * no object of that name (matched ignoring case). */
    bool spawn(RenderDevice& device, const std::filesystem::path& unpackedRoot,
               const LevelRef& level, std::string_view name, const Mat4& transform);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              TreeModel::Pass pass = TreeModel::Pass::All) const;
    void clear();
    usize count() const { return m_spawned.size(); }
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    /** One stood object as the console sees it: its name, and the sphere round it. */
    struct View {
        std::string name;
        Vec3 position{0.0f, 0.0f, 0.0f};
        f32 radius = 0.0f;
    };
    View view(usize index) const;
    /** Marks `id` out with `highlight` as it is drawn, as the developer console's
     * selection; null, or nobody's id, marks nothing. */
    void setHighlight(s32 id, const SelectionHighlight* highlight) {
        m_highlightedId = id;
        m_highlight = highlight;
    }
    usize sourceCount() const { return m_sources.size(); }
    usize bindingCount() const { return m_figures.size(); }

    /** The object at `index` and its descendants as a tree, each at its offset from its
     * parent, the object itself at the root. */
    static TreeInfo treeOf(const WorldLayout& layout, usize index);
    /** The object named `name` in `layout`, ignoring case. */
    static std::optional<usize> objectNamed(const WorldLayout& layout, std::string_view name);

private:
    struct Source {
        std::string level;
        WorldLayout layout;
        ModelSet models;
        TextureSet textures;
        LevelItemArchives items; ///< the realm's, lending the textures the level borrows
    };
    struct Figure {
        std::string level;
        std::string name;
        TreeInfo tree;
        mutable TreeModel model; ///< highlight and mesh draw caches, never continuation state
    };
    Source* sourceOf(const std::filesystem::path& unpackedRoot, const LevelRef& level);
    const Figure* figureOf(const Spawn& spawned) const;

    std::vector<std::unique_ptr<Source>> m_sources;
    std::vector<std::unique_ptr<Figure>> m_figures;
    std::vector<Spawn> m_spawned;
    std::shared_ptr<const u8> m_binding = std::make_shared<const u8>(0);
    s32 m_highlightedId = -1;                        ///< the developer console's selection
    const SelectionHighlight* m_highlight = nullptr; ///< how it is marked out, while drawn
};

} // namespace gdl::game
