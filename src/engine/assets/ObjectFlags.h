#pragma once

#include "engine/core/Types.h"

namespace gdl {

/**
 * The authored flag words the renderer decodes, each defined once. A placed level object
 * and an animation tree node carry the same object-layer word (`objectFlags`); a particle
 * template's flags are mapped onto it by the original when the system is made. The mesh
 * header word comes from the model archive.
 */
namespace object_flags {
inline constexpr u32 kNoDepthTest = 0x40;      ///< drawn without the depth compare
inline constexpr u32 kNoDepthWrite = 0x80;     ///< leaves the depth buffer alone
inline constexpr u32 kSorted = 0x800;          ///< may join the deferred, depth-sorted queue
inline constexpr u32 kNoLightmap = 0x4000;     ///< drawn flat without its lightmap (pbSetDORegs)
inline constexpr u32 kChrome = 0x8000;         ///< environment-mapped by its normals
inline constexpr u32 kSortBack = 0x80000;      ///< deferred key less 10000
inline constexpr u32 kDistanceFade = 0x100400; ///< faded by view depth; deferred key less 30000
inline constexpr u32 kSortBehind = 0x400000;   ///< deferred key less 20000
inline constexpr u32 kAdditive = 0x800000;     ///< added onto the frame
inline constexpr u32 kMultiply = 0x40000000;   ///< multiplied into the frame
inline constexpr u32 kDeferAlways = kAdditive | kMultiply; ///< deferred whenever sortable
inline constexpr u32 kFacingShift = 24;                    ///< the camera-facing mode, four bits
inline constexpr u32 kFacingMask = 0xF;
} // namespace object_flags

namespace mesh_flags {
inline constexpr u32 kAlphaTextured = 0x1; ///< the exporter saw an alpha texture on the object
} // namespace mesh_flags

} // namespace gdl
