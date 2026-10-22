#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"

/** The transcendental functions the simulation uses, computed the same way on every
 * platform. Each C library rounds a few sine, cosine, arc tangent, exponential and
 * blend results differently, which is enough to part two online peers; these are built
 * from plain IEEE double operations and square roots alone, which every compiler rounds
 * identically as long as nothing is contracted into a fused multiply-add (pinned in
 * GdlCompilerOptions.cmake), so the same bits come out on Windows and Linux.
 * Single-precision results are the double result rounded once and lie within an ulp of
 * the correctly rounded value across the ranges the game uses; double results are
 * within a few ulps. Angles beyond a few million radians are reduced crudely, still
 * identically everywhere. */
namespace gdl::portable {

f32 sin(f32 x);
f32 cos(f32 x);
f32 tan(f32 x);
f32 asin(f32 x);
f32 acos(f32 x);
f32 atan(f32 x);
f32 atan2(f32 y, f32 x);
f32 exp(f32 x);
f32 log(f32 x);
f32 pow(f32 base, f32 exponent);
f32 hypot(f32 x, f32 y);
/** std::lerp's algorithm, evaluated in single precision. */
f32 lerp(f32 from, f32 to, f32 amount);

f64 sin(f64 x);
f64 cos(f64 x);
f64 tan(f64 x);
f64 asin(f64 x);
f64 acos(f64 x);
f64 atan(f64 x);
f64 atan2(f64 y, f64 x);
f64 exp(f64 x);
f64 log(f64 x);
f64 pow(f64 base, f64 exponent);
f64 hypot(f64 x, f64 y);
f64 lerp(f64 from, f64 to, f64 amount);

/** glm::rotate, glm::angleAxis and glm::slerp with the functions above. */
Mat4 rotate(const Mat4& m, f32 angle, const Vec3& axis);
Quat angleAxis(f32 angle, const Vec3& axis);
Quat slerp(const Quat& from, const Quat& to, f32 amount);

} // namespace gdl::portable
