#include "engine/world/SelectionHighlight.h"

#include <cmath>

namespace gdl {

f32 SelectionHighlight::rimReach(const Mat4& clip, const Vec3& position, f32 width) {
    constexpr f32 kFallback = 0.1f;
    const f32 depth = (clip * Vec4{position, 1.0f}).w;
    const f32 scale = std::abs(clip[1][1]);
    if (!(depth > 0.0f) || !(scale > 1.0e-6f)) {
        return kFallback;
    }
    // A step d across the view at depth w spans d * scale / w of the two the view's height
    // is in clip space.
    return 2.0f * width * depth / scale;
}

void SelectionHighlight::draw(RenderDevice& device, const Mat4& clip, const Mesh& mesh,
                              const MeshPart& part, const Mat4& placement,
                              const Texture& texture) const {
    if (part.indices.size() < 3) {
        return;
    }
    const Mat3 normals{placement};
    DrawState state;
    state.blend = BlendMode::Alpha;
    state.depthWrite = false;
    state.depthTest = true;
    state.cullBack = true;
    state.mipmaps = false;
    state.alphaTest = DrawState::kTranslucentAlphaTest;
    // Both take their colour from the white mask and their coverage from the part's own
    // texture, so a cutout (a candle on its card) is marked out by its shape, not its card.
    state.maskedTexture = &device.whiteTexture();
    // The rim: each triangle pushed out along its corners' normals and wound the other way,
    // so only its back, past the part's own silhouette, is left to see.
    m_batch.clear();
    m_batch.begin(PrimitiveTopology::TriangleList);
    for (usize triangle = 0; triangle + 2 < part.indices.size(); triangle += 3) {
        for (const usize corner : {0U, 2U, 1U}) {
            const MeshVertex& vertex = mesh.vertices[part.indices[triangle + corner]];
            const Vec3 at{placement * Vec4{vertex.position, 1.0f}};
            Vec3 normal = normals * vertex.normal;
            const f32 length = glm::length(normal);
            if (length > 1.0e-6f) {
                normal /= length;
            }
            m_batch.vertex(at + normal * rimReach(clip, at, m_rimWidth), m_rim, vertex.uv);
        }
    }
    m_batch.end();
    device.draw(m_batch, texture, clip, state);
    // The wash: the part itself, flat coloured, a little nearer the eye than its faces.
    m_batch.clear();
    m_batch.begin(PrimitiveTopology::TriangleList);
    for (const u32 index : part.indices) {
        const MeshVertex& vertex = mesh.vertices[index];
        const Vec3 at{placement * Vec4{vertex.position, 1.0f}};
        m_batch.vertex(at + (m_eye - at) * kLift, m_wash, vertex.uv);
    }
    m_batch.end();
    device.draw(m_batch, texture, clip, state);
}

} // namespace gdl
