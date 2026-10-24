#pragma once

#include <vector>

#include "engine/assets/ObjectFlags.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl {

struct MeshVertex {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 0.0f, 1.0f};
    Vec2 uv{0.0f, 0.0f};
    Vec2 lightmapUv{0.0f, 0.0f};  ///< into the part's lightmap, in its texels, when it has one
    Color color = Color::white(); ///< the lighting baked into the vertex, when the mesh is prelit

    bool operator==(const MeshVertex&) const = default;
};

/** The triangles of a mesh that share one texture (and lightmap). */
struct MeshPart {
    u32 texture = 0;          ///< index into the archive's texture set
    u32 lightmap = 0;         ///< index of the lightmap texture, 0 for none
    std::vector<u32> indices; ///< three per triangle, into Mesh::vertices
};

/** Static triangle geometry with one vertex array and per-texture index ranges. */
struct Mesh {
    std::vector<MeshVertex> vertices;
    std::vector<MeshPart> parts;
    bool prelit = false; ///< the vertices' colours are the lighting the level was built with
    u32 flags = 0;       ///< the archive object's header flags

    /** Whether the exporter marked the object as using an alpha texture. */
    bool alphaTextured() const { return (flags & mesh_flags::kAlphaTextured) != 0; }

    usize triangleCount() const {
        usize count = 0;
        for (const MeshPart& part : parts) {
            count += part.indices.size() / 3;
        }
        return count;
    }
};

/** The ways a part's coordinates never leave one edge of its texture: pinned there, it
 * samples that edge's single column (across) or row (down) over its whole face. */
struct EdgePin {
    bool across = false;
    bool down = false;
};

/** Where `part` is pinned to an edge of its texture: its coordinates hold one value that
 * way, at or past the edge (a clamped texture shows its edge from there on). */
inline EdgePin edgePinOf(const Mesh& mesh, const MeshPart& part) {
    if (part.indices.empty()) {
        return {};
    }
    Vec2 low = mesh.vertices[part.indices.front()].uv;
    Vec2 high = low;
    for (const u32 index : part.indices) {
        const Vec2& uv = mesh.vertices[index].uv;
        low = glm::min(low, uv);
        high = glm::max(high, uv);
    }
    constexpr f32 kSlack = 1.0f / 1024.0f;
    const auto pinned = [](f32 lowest, f32 highest) {
        return highest - lowest <= kSlack && (highest <= kSlack || lowest >= 1.0f - kSlack);
    };
    return {.across = pinned(low.x, high.x), .down = pinned(low.y, high.y)};
}

} // namespace gdl
