#include "engine/math/PortableMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace gdl::portable {
namespace {

constexpr f64 kHalfPi = std::numbers::pi / 2;      // the double nearest pi/2
constexpr f64 kHalfPiTail = 6.123233995736766e-17; // pi/2 minus it
constexpr f64 kPi = std::numbers::pi;
constexpr f64 kPiTail = 1.2246467991473532e-16;
constexpr f64 kTwoOverPi = 0.6366197723675814;
constexpr f64 kLn2 = std::numbers::ln2;
constexpr f64 kLn2Tail = 2.3190468138462996e-17;
constexpr f64 kInvLn2 = std::numbers::log2e;
constexpr f64 kHalfSqrt2 = 0.7071067811865476;
constexpr f64 kSplitter = 134217729.0;     // 2^27 + 1 splits a double into two exact halves
constexpr f64 kCrudeReduction = 1048576.0; // beyond this many radians the quadrant is a guess
constexpr f64 kInfinity = std::numeric_limits<f64>::infinity();
constexpr f64 kNan = std::numeric_limits<f64>::quiet_NaN();

/** a * b as an exact sum of two doubles (Dekker's product, no fused multiply-add). */
struct Product {
    f64 high;
    f64 low;
};
Product exactProduct(f64 a, f64 b) {
    const f64 high = a * b;
    const f64 splitA = kSplitter * a;
    const f64 highA = splitA - (splitA - a);
    const f64 lowA = a - highA;
    const f64 splitB = kSplitter * b;
    const f64 highB = splitB - (splitB - b);
    const f64 lowB = b - highB;
    const f64 low = ((highA * highB - high) + highA * lowB + lowA * highB) + lowA * lowB;
    return {high, low};
}

/** x - n * (high + tail), the product taken exactly so the remainder keeps its bits. */
f64 remainder(f64 x, f64 n, f64 high, f64 tail) {
    const auto product = exactProduct(n, high);
    return ((x - product.high) - product.low) - n * tail;
}

/** Horner's rule over z for terms[0] + z * (terms[1] + z * (...)). */
template <usize N> f64 polynomial(const std::array<f64, N>& terms, f64 z) {
    f64 sum = terms[N - 1];
    for (usize index = N - 1; index > 0; --index) {
        sum = terms[index - 1] + z * sum;
    }
    return sum;
}

// Taylor series on |r| <= pi/4 (sine, cosine), |r| <= tan(pi/16) (arc tangent),
// |r| <= ln(2)/2 (exponential) and |s| <= 0.1716 (logarithm); each is carried far
// enough that the dropped tail is below a double's precision.
constexpr auto kSineTerms = std::to_array<f64>(
    {-1.0 / 6.0, 1.0 / 120.0, -1.0 / 5040.0, 1.0 / 362880.0, -1.0 / 39916800.0, 1.0 / 6227020800.0,
     -1.0 / 1307674368000.0, 1.0 / 355687428096000.0, -1.0 / 121645100408832000.0});
constexpr auto kCosineTerms = std::to_array<f64>(
    {-1.0 / 2.0, 1.0 / 24.0, -1.0 / 720.0, 1.0 / 40320.0, -1.0 / 3628800.0, 1.0 / 479001600.0,
     -1.0 / 87178291200.0, 1.0 / 20922789888000.0, -1.0 / 6402373705728000.0});
constexpr auto kArcTangentTerms =
    std::to_array<f64>({-1.0 / 3.0, 1.0 / 5.0, -1.0 / 7.0, 1.0 / 9.0, -1.0 / 11.0, 1.0 / 13.0,
                        -1.0 / 15.0, 1.0 / 17.0, -1.0 / 19.0, 1.0 / 21.0, -1.0 / 23.0});
constexpr auto kExponentialTerms =
    std::to_array<f64>({1.0 / 2.0, 1.0 / 6.0, 1.0 / 24.0, 1.0 / 120.0, 1.0 / 720.0, 1.0 / 5040.0,
                        1.0 / 40320.0, 1.0 / 362880.0, 1.0 / 3628800.0, 1.0 / 39916800.0,
                        1.0 / 479001600.0, 1.0 / 6227020800.0, 1.0 / 87178291200.0});
constexpr auto kLogarithmTerms =
    std::to_array<f64>({1.0 / 3.0, 1.0 / 5.0, 1.0 / 7.0, 1.0 / 9.0, 1.0 / 11.0, 1.0 / 13.0,
                        1.0 / 15.0, 1.0 / 17.0, 1.0 / 19.0, 1.0 / 21.0, 1.0 / 23.0});

f64 sineKernel(f64 r) {
    const f64 z = r * r;
    return r + r * (z * polynomial(kSineTerms, z));
}
f64 cosineKernel(f64 r) {
    const f64 z = r * r;
    return 1.0 + z * polynomial(kCosineTerms, z);
}

struct Reduced {
    f64 r;
    u32 quadrant;
};
/** x as r + quadrant * pi/2 with |r| <= pi/4. */
Reduced reduceByHalfPi(f64 x) {
    if (!(std::abs(x) <= kCrudeReduction)) {
        if (!std::isfinite(x)) {
            return {kNan, 0};
        }
        x = std::fmod(x, 2.0 * kPi);
    }
    const f64 n = std::nearbyint(x * kTwoOverPi);
    const auto quadrant = static_cast<u32>(static_cast<s64>(n)) & 3U;
    return {remainder(x, n, kHalfPi, kHalfPiTail), quadrant};
}

f64 sine(f64 x) {
    if (x == 0.0) {
        return x; // keeps the sign of zero
    }
    const auto [r, quadrant] = reduceByHalfPi(x);
    switch (quadrant) {
    case 0: return sineKernel(r);
    case 1: return cosineKernel(r);
    case 2: return -sineKernel(r);
    default: return -cosineKernel(r);
    }
}
f64 cosine(f64 x) {
    const auto [r, quadrant] = reduceByHalfPi(x);
    switch (quadrant) {
    case 0: return cosineKernel(r);
    case 1: return -sineKernel(r);
    case 2: return -cosineKernel(r);
    default: return sineKernel(r);
    }
}
f64 tangent(f64 x) {
    if (x == 0.0) {
        return x;
    }
    const auto [r, quadrant] = reduceByHalfPi(x);
    return (quadrant & 1U) == 0 ? sineKernel(r) / cosineKernel(r)
                                : -cosineKernel(r) / sineKernel(r);
}

/** Arc tangent of 0 <= a <= 1. */
f64 arcTangentUnit(f64 a) {
    // Halving the angle twice brings the argument under tan(pi/16).
    a = a / (1.0 + std::sqrt(1.0 + a * a));
    a = a / (1.0 + std::sqrt(1.0 + a * a));
    const f64 z = a * a;
    return 4.0 * (a + a * (z * polynomial(kArcTangentTerms, z)));
}
f64 arcTangent(f64 x) {
    if (std::isnan(x)) {
        return x;
    }
    const f64 a = std::abs(x);
    if (std::isinf(a)) {
        return std::copysign(kHalfPi, x);
    }
    const f64 r = a > 1.0 ? (kHalfPi - arcTangentUnit(1.0 / a)) + kHalfPiTail : arcTangentUnit(a);
    return std::copysign(r, x);
}
f64 arcSine(f64 x) {
    if (std::isnan(x)) {
        return x;
    }
    if (std::abs(x) > 1.0) {
        return kNan;
    }
    return 2.0 * arcTangent(x / (1.0 + std::sqrt((1.0 - x) * (1.0 + x))));
}
f64 arcCosine(f64 x) {
    if (std::isnan(x)) {
        return x;
    }
    if (std::abs(x) > 1.0) {
        return kNan;
    }
    // Exact at one; the division by zero at minus one gives the arc tangent of infinity.
    return 2.0 * arcTangent(std::sqrt((1.0 - x) / (1.0 + x)));
}
f64 arcTangent2(f64 y, f64 x) {
    if (std::isnan(x) || std::isnan(y)) {
        return kNan;
    }
    if (y == 0.0) {
        return x > 0.0 || (x == 0.0 && !std::signbit(x)) ? y : std::copysign(kPi, y);
    }
    if (x == 0.0) {
        return std::copysign(kHalfPi, y);
    }
    if (std::isinf(x)) {
        if (std::isinf(y)) {
            return std::copysign(x > 0.0 ? 0.25 * kPi : 0.75 * kPi, y);
        }
        return std::copysign(x > 0.0 ? 0.0 : kPi, y);
    }
    if (std::isinf(y)) {
        return std::copysign(kHalfPi, y);
    }
    const f64 ay = std::abs(y);
    const f64 ax = std::abs(x);
    f64 t = ay <= ax ? arcTangentUnit(ay / ax) : (kHalfPi - arcTangentUnit(ax / ay)) + kHalfPiTail;
    if (x < 0.0) {
        t = (kPi - t) + kPiTail;
    }
    return std::copysign(t, y);
}

f64 exponential(f64 x) {
    if (std::isnan(x)) {
        return x;
    }
    if (x > 709.782712893384) {
        return kInfinity;
    }
    if (x < -745.1332191019412) {
        return 0.0;
    }
    const f64 n = std::nearbyint(x * kInvLn2);
    const f64 r = remainder(x, n, kLn2, kLn2Tail);
    const f64 e = 1.0 + r + (r * r) * polynomial(kExponentialTerms, r);
    return std::ldexp(e, static_cast<int>(n));
}
f64 logarithm(f64 x) {
    if (std::isnan(x)) {
        return x;
    }
    if (x < 0.0) {
        return kNan;
    }
    if (x == 0.0) {
        return -kInfinity;
    }
    if (std::isinf(x)) {
        return x;
    }
    int exponent = 0;
    f64 mantissa = std::frexp(x, &exponent);
    if (mantissa < kHalfSqrt2) {
        mantissa *= 2.0;
        exponent -= 1;
    }
    const f64 s = (mantissa - 1.0) / (mantissa + 1.0);
    const f64 z = s * s;
    const f64 half = s + s * (z * polynomial(kLogarithmTerms, z));
    const auto scale = static_cast<f64>(exponent);
    return (scale * kLn2 + (half + half)) + scale * kLn2Tail;
}
f64 power(f64 base, f64 exponent) {
    if (exponent == 0.0 || base == 1.0) {
        return 1.0;
    }
    if (std::isnan(base) || std::isnan(exponent)) {
        return kNan;
    }
    const bool integral = std::isfinite(exponent) && exponent == std::trunc(exponent);
    const bool odd = integral && std::fmod(exponent, 2.0) != 0.0;
    if (base == 0.0) {
        if (exponent < 0.0) {
            return odd ? std::copysign(kInfinity, base) : kInfinity;
        }
        return odd ? base : 0.0;
    }
    if (std::isinf(exponent)) {
        const f64 magnitude = std::abs(base);
        if (magnitude == 1.0) {
            return 1.0;
        }
        return (magnitude > 1.0) == (exponent > 0.0) ? kInfinity : 0.0;
    }
    if (std::isinf(base)) {
        if (base > 0.0) {
            return exponent > 0.0 ? kInfinity : 0.0;
        }
        if (exponent > 0.0) {
            return odd ? -kInfinity : kInfinity;
        }
        return odd ? -0.0 : 0.0;
    }
    f64 magnitude = base;
    f64 sign = 1.0;
    if (base < 0.0) {
        if (!integral) {
            return kNan;
        }
        magnitude = -base;
        sign = odd ? -1.0 : 1.0;
    }
    return sign * exponential(exponent * logarithm(magnitude));
}
f64 hypotenuse(f64 x, f64 y) {
    f64 a = std::abs(x);
    f64 b = std::abs(y);
    if (std::isinf(a) || std::isinf(b)) {
        return kInfinity;
    }
    if (std::isnan(a) || std::isnan(b)) {
        return kNan;
    }
    if (a < b) {
        std::swap(a, b);
    }
    if (a == 0.0) {
        return 0.0;
    }
    const f64 ratio = b / a;
    return a * std::sqrt(1.0 + ratio * ratio);
}
template <class T> T blend(T from, T to, T amount) {
    if ((from <= T{0} && to >= T{0}) || (from >= T{0} && to <= T{0})) {
        return amount * to + (T{1} - amount) * from;
    }
    if (amount == T{1}) {
        return to;
    }
    const T x = from + amount * (to - from);
    return (amount > T{1}) == (to > from) ? std::max(to, x) : std::min(to, x);
}

} // namespace

f32 sin(f32 x) {
    return static_cast<f32>(sine(x));
}
f32 cos(f32 x) {
    return static_cast<f32>(cosine(x));
}
f32 tan(f32 x) {
    return static_cast<f32>(tangent(x));
}
f32 asin(f32 x) {
    return static_cast<f32>(arcSine(x));
}
f32 acos(f32 x) {
    return static_cast<f32>(arcCosine(x));
}
f32 atan(f32 x) {
    return static_cast<f32>(arcTangent(x));
}
f32 atan2(f32 y, f32 x) {
    return static_cast<f32>(arcTangent2(y, x));
}
f32 exp(f32 x) {
    return static_cast<f32>(exponential(x));
}
f32 log(f32 x) {
    return static_cast<f32>(logarithm(x));
}
f32 pow(f32 base, f32 exponent) {
    return static_cast<f32>(power(base, exponent));
}
f32 hypot(f32 x, f32 y) {
    return static_cast<f32>(hypotenuse(x, y));
}
f32 lerp(f32 from, f32 to, f32 amount) {
    return blend(from, to, amount);
}

f64 sin(f64 x) {
    return sine(x);
}
f64 cos(f64 x) {
    return cosine(x);
}
f64 tan(f64 x) {
    return tangent(x);
}
f64 asin(f64 x) {
    return arcSine(x);
}
f64 acos(f64 x) {
    return arcCosine(x);
}
f64 atan(f64 x) {
    return arcTangent(x);
}
f64 atan2(f64 y, f64 x) {
    return arcTangent2(y, x);
}
f64 exp(f64 x) {
    return exponential(x);
}
f64 log(f64 x) {
    return logarithm(x);
}
f64 pow(f64 base, f64 exponent) {
    return power(base, exponent);
}
f64 hypot(f64 x, f64 y) {
    return hypotenuse(x, y);
}
f64 lerp(f64 from, f64 to, f64 amount) {
    return blend(from, to, amount);
}

Mat4 rotate(const Mat4& m, f32 angle, const Vec3& axis) {
    const f32 c = cos(angle);
    const f32 s = sin(angle);
    const Vec3 unit = glm::normalize(axis);
    const Vec3 temp = (1.0f - c) * unit;
    Mat4 turn{};
    turn[0][0] = c + temp[0] * unit[0];
    turn[0][1] = temp[0] * unit[1] + s * unit[2];
    turn[0][2] = temp[0] * unit[2] - s * unit[1];
    turn[1][0] = temp[1] * unit[0] - s * unit[2];
    turn[1][1] = c + temp[1] * unit[1];
    turn[1][2] = temp[1] * unit[2] + s * unit[0];
    turn[2][0] = temp[2] * unit[0] + s * unit[1];
    turn[2][1] = temp[2] * unit[1] - s * unit[0];
    turn[2][2] = c + temp[2] * unit[2];
    Mat4 result{};
    result[0] = m[0] * turn[0][0] + m[1] * turn[0][1] + m[2] * turn[0][2];
    result[1] = m[0] * turn[1][0] + m[1] * turn[1][1] + m[2] * turn[1][2];
    result[2] = m[0] * turn[2][0] + m[1] * turn[2][1] + m[2] * turn[2][2];
    result[3] = m[3];
    return result;
}
Quat angleAxis(f32 angle, const Vec3& axis) {
    const f32 half = angle * 0.5f;
    return Quat{cos(half), axis * sin(half)};
}
Quat slerp(const Quat& from, const Quat& to, f32 amount) {
    Quat z = to;
    f32 cosTheta = glm::dot(from, to);
    if (cosTheta < 0.0f) {
        z = -to;
        cosTheta = -cosTheta;
    }
    if (cosTheta > 1.0f - std::numeric_limits<f32>::epsilon()) {
        return Quat{glm::mix(from.w, z.w, amount), glm::mix(from.x, z.x, amount),
                    glm::mix(from.y, z.y, amount), glm::mix(from.z, z.z, amount)};
    }
    const f32 angle = acos(cosTheta);
    return (sin((1.0f - amount) * angle) * from + sin(amount * angle) * z) / sin(angle);
}

} // namespace gdl::portable
