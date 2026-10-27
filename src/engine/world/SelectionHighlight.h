#pragma once
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/Mesh.h"
#include "engine/render/RenderDevice.h"

namespace gdl {

/**
 * How a thing picked in the developer console is marked out, the way the level editor marks
 * its selection: a bright orange rim round its silhouette and a dull orange wash over the
 * faces in view. The rim is the part's faces pushed out along their normals and wound the
 * other way, so with the back faces culled only the overhang past the silhouette shows, and
 * only where nothing stands before it. The wash is the part again, flat coloured and blended
 * by its texture's alpha, lifted a little toward the eye so it lies over its own faces alone.
 */
class SelectionHighlight {
public:
    static constexpr Color kRim = Color::rgba(255, 140, 31);      ///< the editor's orange
    static constexpr Color kWash = Color::rgba(255, 140, 31, 51); ///< a fifth of it, over the faces
    static constexpr f32 kRimWidth = 1.0f / 192.0f; ///< of the view's height, at any distance
    static constexpr f32 kLift = 0.005f;            ///< of the way to the eye

    explicit SelectionHighlight(const Vec3& eye) : m_eye(eye) {}
    /** The same marking in other colours and another rim width: an aura, say. */
    SelectionHighlight(const Vec3& eye, Color rim, Color wash, f32 rimWidth = kRimWidth)
        : m_eye(eye), m_rim(rim), m_wash(wash), m_rimWidth(rimWidth) {}

    const Vec3& eye() const { return m_eye; }
    Color rim() const { return m_rim; }
    Color wash() const { return m_wash; }
    f32 rimWidth() const { return m_rimWidth; }

    /** Draws the rim and the wash of `part` of `mesh` placed by `placement`, the clear
     * texels of `texture` cut out of the wash. */
    void draw(RenderDevice& device, const Mat4& clip, const Mesh& mesh, const MeshPart& part,
              const Mat4& placement, const Texture& texture) const;

    /** How far a point at `position` is pushed out for a rim `width` of the view's height:
     * that share at the point's depth, in world units. */
    static f32 rimReach(const Mat4& clip, const Vec3& position, f32 width = kRimWidth);

private:
    Vec3 m_eye;
    Color m_rim = kRim;
    Color m_wash = kWash;
    f32 m_rimWidth = kRimWidth;
    mutable ImmediateBatch m_batch;
};

} // namespace gdl
