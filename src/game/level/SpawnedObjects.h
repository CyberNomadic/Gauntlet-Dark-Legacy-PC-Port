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
public:
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
    usize sourceCount() const { return m_sources.size(); }

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
    struct Spawned {
        std::string name;
        TreeInfo tree;
        TreeModel model;
        Mat4 transform{1.0f};
    };
    Source* sourceOf(const std::filesystem::path& unpackedRoot, const LevelRef& level);

    std::vector<std::unique_ptr<Source>> m_sources;
    std::vector<std::unique_ptr<Spawned>> m_spawned;
};

} // namespace gdl::game
