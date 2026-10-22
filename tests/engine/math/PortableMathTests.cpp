#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/Sha256.h"
#include "engine/math/PortableMath.h"

namespace {
using namespace gdl;

/** Operands over the ranges the game uses, in a fixed order. */
class Sequence {
public:
    f32 single(f32 low, f32 high) {
        return low + (high - low) * (static_cast<f32>(next() >> 8) * (1.0f / 16777216.0f));
    }
    f64 wide(f64 low, f64 high) {
        return low + (high - low) * (static_cast<f64>(next()) * (1.0 / 4294967296.0));
    }

private:
    u32 next() {
        m_state = m_state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<u32>(m_state >> 32);
    }
    u64 m_state = 0x9E3779B97F4A7C15ULL;
};

/** How many representable values apart two finite results are. */
u64 ulpsApart(f32 a, f32 b) {
    if (a == b) {
        return 0;
    }
    if (std::isnan(a) || std::isnan(b) || std::isinf(a) || std::isinf(b)) {
        return std::numeric_limits<u64>::max();
    }
    const auto order = [](f32 value) {
        const auto bits = static_cast<s64>(std::bit_cast<s32>(value));
        return bits < 0 ? std::numeric_limits<s32>::min() - bits : bits;
    };
    const s64 first = order(a);
    const s64 second = order(b);
    return static_cast<u64>(first > second ? first - second : second - first);
}
u64 ulpsApart(f64 a, f64 b) {
    if (a == b) {
        return 0;
    }
    if (std::isnan(a) || std::isnan(b) || std::isinf(a) || std::isinf(b)) {
        return std::numeric_limits<u64>::max();
    }
    const auto order = [](f64 value) {
        const auto bits = std::bit_cast<s64>(value);
        return bits < 0 ? std::numeric_limits<s64>::min() - bits : bits;
    };
    const s64 first = order(a);
    const s64 second = order(b);
    return static_cast<u64>(first > second ? first - second : second - first);
}

struct Worst {
    u64 ulps = 0;
    f64 input = 0;
    f64 other = 0;
    void note(u64 distance, f64 x, f64 y = 0) {
        if (distance > ulps) {
            ulps = distance;
            input = x;
            other = y;
        }
    }
};

constexpr u32 kSamples = 100000;
constexpr f32 kFourPi = 4.0f * std::numbers::pi_v<f32>;
constexpr f64 kFourPiWide = 4.0 * std::numbers::pi;
constexpr std::array<std::string_view, 11> kNames{"sin",   "cos", "tan", "asin", "acos", "atan",
                                                  "atan2", "exp", "log", "pow",  "hypot"};
} // namespace

TEST_CASE("portable single-precision functions are within an ulp of the correctly rounded value",
          "[math][portable-math][determinism]") {
    // The C library's double result rounded to single precision is correctly rounded
    // except in rare double-rounding cases, so it serves as the reference.
    Sequence sequence;
    std::array<Worst, 11> worst{};
    for (u32 sample = 0; sample < kSamples; ++sample) {
        const f32 angle = sequence.single(-kFourPi, kFourPi);
        const f32 unit = sequence.single(-1.0f, 1.0f);
        const f32 length = sequence.single(0.001f, 10000.0f);
        const f32 exponent = sequence.single(-20.0f, 20.0f);
        const f32 other = sequence.single(-100.0f, 100.0f);
        const f32 power = sequence.single(-3.0f, 3.0f);
        const auto check = [&](usize slot, f32 got, f64 reference, f64 x, f64 y = 0) {
            worst[slot].note(ulpsApart(got, static_cast<f32>(reference)), x, y);
        };
        check(0, portable::sin(angle), std::sin(static_cast<f64>(angle)), angle);
        check(1, portable::cos(angle), std::cos(static_cast<f64>(angle)), angle);
        check(2, portable::tan(angle), std::tan(static_cast<f64>(angle)), angle);
        check(3, portable::asin(unit), std::asin(static_cast<f64>(unit)), unit);
        check(4, portable::acos(unit), std::acos(static_cast<f64>(unit)), unit);
        check(5, portable::atan(other), std::atan(static_cast<f64>(other)), other);
        check(6, portable::atan2(other, angle),
              std::atan2(static_cast<f64>(other), static_cast<f64>(angle)), other, angle);
        check(7, portable::exp(exponent), std::exp(static_cast<f64>(exponent)), exponent);
        check(8, portable::log(length), std::log(static_cast<f64>(length)), length);
        check(9, portable::pow(length, power),
              std::pow(static_cast<f64>(length), static_cast<f64>(power)), length, power);
        check(10, portable::hypot(other, angle),
              std::hypot(static_cast<f64>(other), static_cast<f64>(angle)), other, angle);
    }
    for (usize slot = 0; slot < worst.size(); ++slot) {
        CAPTURE(kNames[slot], worst[slot].input, worst[slot].other);
        CHECK(worst[slot].ulps <= 1);
    }
}

TEST_CASE("portable double-precision functions stay within a few ulps of the C library",
          "[math][portable-math][determinism]") {
    Sequence sequence;
    std::array<Worst, 11> worst{};
    for (u32 sample = 0; sample < kSamples; ++sample) {
        const f64 angle = sequence.wide(-kFourPiWide, kFourPiWide);
        const f64 unit = sequence.wide(-1.0, 1.0);
        const f64 length = sequence.wide(0.001, 10000.0);
        const f64 exponent = sequence.wide(-20.0, 20.0);
        const f64 other = sequence.wide(-100.0, 100.0);
        const f64 power = sequence.wide(-3.0, 3.0);
        worst[0].note(ulpsApart(portable::sin(angle), std::sin(angle)), angle);
        worst[1].note(ulpsApart(portable::cos(angle), std::cos(angle)), angle);
        worst[2].note(ulpsApart(portable::tan(angle), std::tan(angle)), angle);
        worst[3].note(ulpsApart(portable::asin(unit), std::asin(unit)), unit);
        worst[4].note(ulpsApart(portable::acos(unit), std::acos(unit)), unit);
        worst[5].note(ulpsApart(portable::atan(other), std::atan(other)), other);
        worst[6].note(ulpsApart(portable::atan2(other, angle), std::atan2(other, angle)), other,
                      angle);
        worst[7].note(ulpsApart(portable::exp(exponent), std::exp(exponent)), exponent);
        worst[8].note(ulpsApart(portable::log(length), std::log(length)), length);
        worst[9].note(ulpsApart(portable::pow(length, power), std::pow(length, power)), length,
                      power);
        worst[10].note(ulpsApart(portable::hypot(other, angle), std::hypot(other, angle)), other,
                       angle);
    }
    // The arc sine and cosine pass through a square root and a division before the arc
    // tangent; a power is an exponential of a product, so its error grows with the exponent.
    constexpr std::array<u64, 11> kBounds{4, 4, 8, 8, 8, 4, 4, 4, 4, 64, 4};
    for (usize slot = 0; slot < worst.size(); ++slot) {
        CAPTURE(kNames[slot], worst[slot].input, worst[slot].other, worst[slot].ulps);
        CHECK(worst[slot].ulps <= kBounds[slot]);
    }
}

TEST_CASE("portable lerp keeps the standard's guarantees", "[math][portable-math][determinism]") {
    // Exact at both ends, monotonic between them and never outside them, in both
    // precisions and whichever way the ends are ordered or signed.
    Sequence sequence;
    for (u32 sample = 0; sample < 2000; ++sample) {
        const f32 from = sequence.single(-100.0f, 100.0f);
        const f32 to = sequence.single(-100.0f, 100.0f);
        CAPTURE(from, to);
        CHECK(portable::lerp(from, to, 0.0f) == from);
        CHECK(portable::lerp(from, to, 1.0f) == to);
        f32 previous = from;
        for (u32 step = 1; step <= 64; ++step) {
            const f32 value = portable::lerp(from, to, static_cast<f32>(step) / 64.0f);
            CHECK(value >= std::min(from, to));
            CHECK(value <= std::max(from, to));
            CHECK((from <= to ? value >= previous : value <= previous));
            previous = value;
        }
        const f64 fromWide = sequence.wide(-100.0, 100.0);
        const f64 toWide = sequence.wide(-100.0, 100.0);
        CHECK(portable::lerp(fromWide, toWide, 0.0) == fromWide);
        CHECK(portable::lerp(fromWide, toWide, 1.0) == toWide);
        const f64 middle = portable::lerp(fromWide, toWide, 0.5);
        CHECK(middle >= std::min(fromWide, toWide));
        CHECK(middle <= std::max(fromWide, toWide));
    }
}

TEST_CASE("portable functions honour the special values", "[math][portable-math][determinism]") {
    constexpr f32 kInfinity = std::numeric_limits<f32>::infinity();
    constexpr f32 kNan = std::numeric_limits<f32>::quiet_NaN();
    constexpr f32 kQuarterTurn = std::numbers::pi_v<f32> / 2;
    CHECK(std::bit_cast<u32>(portable::sin(-0.0f)) == std::bit_cast<u32>(-0.0f));
    CHECK(std::bit_cast<u32>(portable::sin(0.0f)) == 0);
    CHECK(portable::cos(0.0f) == 1.0f);
    CHECK(std::bit_cast<u32>(portable::tan(-0.0f)) == std::bit_cast<u32>(-0.0f));
    CHECK(std::isnan(portable::sin(kInfinity)));
    CHECK(std::isnan(portable::cos(kNan)));
    CHECK(std::isfinite(portable::sin(1.0e7f)));
    CHECK(portable::sin(1.0e7f) == portable::sin(1.0e7f));
    CHECK(portable::atan(kInfinity) == kQuarterTurn);
    CHECK(portable::atan(-kInfinity) == -kQuarterTurn);
    CHECK(portable::asin(1.0f) == kQuarterTurn);
    CHECK(portable::asin(-1.0f) == -kQuarterTurn);
    CHECK(std::isnan(portable::asin(1.5f)));
    CHECK(portable::acos(1.0f) == 0.0f);
    CHECK(portable::acos(-1.0f) == std::numbers::pi_v<f32>);
    CHECK(portable::acos(0.0f) == kQuarterTurn);
    for (const f32 y : {0.0f, -0.0f, 1.0f, -1.0f, kInfinity, -kInfinity}) {
        for (const f32 x : {0.0f, -0.0f, 1.0f, -1.0f, kInfinity, -kInfinity}) {
            CAPTURE(y, x);
            CHECK(std::bit_cast<u32>(portable::atan2(y, x)) ==
                  std::bit_cast<u32>(std::atan2(y, x)));
        }
    }
    CHECK(std::isnan(portable::atan2(kNan, 1.0f)));
    CHECK(portable::exp(0.0f) == 1.0f);
    CHECK(portable::exp(-kInfinity) == 0.0f);
    CHECK(portable::exp(kInfinity) == kInfinity);
    CHECK(portable::exp(100.0f) == kInfinity);
    CHECK(portable::exp(-110.0f) == 0.0f);
    CHECK(portable::log(1.0f) == 0.0f);
    CHECK(portable::log(0.0f) == -kInfinity);
    CHECK(std::isnan(portable::log(-1.0f)));
    CHECK(portable::log(kInfinity) == kInfinity);
    CHECK(portable::pow(2.5f, 0.0f) == 1.0f);
    CHECK(portable::pow(1.0f, kNan) == 1.0f);
    CHECK(portable::pow(0.0f, -1.0f) == kInfinity);
    CHECK(portable::pow(-0.0f, -1.0f) == -kInfinity);
    CHECK(portable::pow(0.0f, 2.0f) == 0.0f);
    CHECK(portable::pow(-2.0f, 3.0f) == -8.0f);
    CHECK(portable::pow(-2.0f, 2.0f) == 4.0f);
    CHECK(std::isnan(portable::pow(-2.0f, 0.5f)));
    CHECK(portable::pow(2.0f, kInfinity) == kInfinity);
    CHECK(portable::pow(0.5f, kInfinity) == 0.0f);
    CHECK(portable::pow(-kInfinity, 3.0f) == -kInfinity);
    CHECK(portable::hypot(3.0f, 4.0f) == 5.0f);
    CHECK(portable::hypot(0.0f, 0.0f) == 0.0f);
    CHECK(portable::hypot(kInfinity, kNan) == kInfinity);
    CHECK(std::isnan(portable::hypot(1.0f, kNan)));
    CHECK(portable::lerp(2.0f, 8.0f, 0.0f) == 2.0f);
    CHECK(portable::lerp(2.0f, 8.0f, 1.0f) == 8.0f);
    CHECK(portable::lerp(2.0f, 8.0f, 0.5f) == 5.0f);
    CHECK(portable::lerp(-3.0f, 3.0f, 0.5f) == 0.0f);
    CHECK(portable::lerp(1.0f, 1.0f, 0.5f) == 1.0f);
    CHECK(portable::lerp(0.0, 10.0, 0.25) == 2.5);
}

TEST_CASE("portable results have one bit pattern everywhere",
          "[math][portable-math][determinism]") {
    // Recorded on Windows with MSVC and confirmed with GCC on Linux; a change here
    // means the functions no longer agree across platforms, or were changed on purpose.
    constexpr std::string_view kExpected =
        "279980599c7148cb546450a7903316b1e10b31c61d9dd97bbe783b509a3d2800";
    Sha256 digest;
    const auto note = [&](auto value) {
        using Bits = std::conditional_t<sizeof(value) == 4, u32, u64>;
        const auto bits = std::bit_cast<Bits>(value);
        std::array<u8, sizeof(Bits)> bytes{};
        for (usize i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<u8>(bits >> (8 * i));
        }
        digest.update(bytes);
    };
    Sequence sequence;
    for (u32 sample = 0; sample < 4096; ++sample) {
        const f32 angle = sequence.single(-kFourPi, kFourPi);
        const f32 unit = sequence.single(-1.0f, 1.0f);
        const f32 length = sequence.single(0.001f, 10000.0f);
        const f32 exponent = sequence.single(-20.0f, 20.0f);
        const f32 other = sequence.single(-100.0f, 100.0f);
        const f32 amount = sequence.single(0.0f, 1.0f);
        const f32 power = sequence.single(-3.0f, 3.0f);
        note(portable::sin(angle));
        note(portable::cos(angle));
        note(portable::tan(angle));
        note(portable::asin(unit));
        note(portable::acos(unit));
        note(portable::atan(other));
        note(portable::atan2(other, angle));
        note(portable::exp(exponent));
        note(portable::log(length));
        note(portable::pow(length, power));
        note(portable::hypot(other, angle));
        note(portable::lerp(other, angle, amount));
        const Mat4 turned = portable::rotate(Mat4{1.0f}, angle, Vec3{unit, amount, 1.0f - amount});
        const Quat spun = portable::angleAxis(angle, glm::normalize(Vec3{amount, unit, 0.5f}));
        const Quat blended =
            portable::slerp(spun, portable::angleAxis(other, Vec3{0, 1, 0}), amount);
        for (s32 i = 0; i < 4; ++i) {
            note(blended[i]);
            for (s32 j = 0; j < 4; ++j) {
                note(turned[i][j]);
            }
        }
        for (s32 i = 0; i < 4; ++i) {
            note(spun[i]);
        }
    }
    for (u32 sample = 0; sample < 4096; ++sample) {
        const f64 angle = sequence.wide(-kFourPiWide, kFourPiWide);
        const f64 unit = sequence.wide(-1.0, 1.0);
        const f64 length = sequence.wide(0.001, 10000.0);
        const f64 exponent = sequence.wide(-20.0, 20.0);
        const f64 other = sequence.wide(-100.0, 100.0);
        const f64 amount = sequence.wide(0.0, 1.0);
        const f64 power = sequence.wide(-3.0, 3.0);
        note(portable::sin(angle));
        note(portable::cos(angle));
        note(portable::tan(angle));
        note(portable::asin(unit));
        note(portable::acos(unit));
        note(portable::atan(other));
        note(portable::atan2(other, angle));
        note(portable::exp(exponent));
        note(portable::log(length));
        note(portable::pow(length, power));
        note(portable::hypot(other, angle));
        note(portable::lerp(other, angle, amount));
    }
    const std::string actual = digest.hex();
    INFO(actual);
    CHECK(actual == kExpected);
}

TEST_CASE("portable rotate, angleAxis and slerp match glm's",
          "[math][portable-math][determinism]") {
    Sequence sequence;
    f32 worst = 0;
    for (u32 sample = 0; sample < 2000; ++sample) {
        const f32 angle = sequence.single(-kFourPi, kFourPi);
        const Vec3 axis =
            glm::normalize(Vec3{sequence.single(-1.0f, 1.0f), sequence.single(-1.0f, 1.0f),
                                sequence.single(0.1f, 1.0f)});
        const Mat4 base = glm::translate(Mat4{1.0f}, Vec3{sequence.single(-50.0f, 50.0f),
                                                          sequence.single(-50.0f, 50.0f),
                                                          sequence.single(-50.0f, 50.0f)});
        const Mat4 expected = glm::rotate(base, angle, axis);
        const Mat4 got = portable::rotate(base, angle, axis);
        for (s32 i = 0; i < 4; ++i) {
            for (s32 j = 0; j < 4; ++j) {
                worst = std::max(worst, std::abs(expected[i][j] - got[i][j]));
            }
        }
        const Quat spun = glm::angleAxis(angle, axis);
        const Quat spunPortable = portable::angleAxis(angle, axis);
        const f32 other = sequence.single(-kFourPi, kFourPi);
        const f32 amount = sequence.single(0.0f, 1.0f);
        const Quat target = glm::angleAxis(other, Vec3{0, 1, 0});
        const Quat blended = glm::slerp(spun, target, amount);
        const Quat blendedPortable = portable::slerp(spunPortable, target, amount);
        for (s32 i = 0; i < 4; ++i) {
            worst = std::max(worst, std::abs(spun[i] - spunPortable[i]));
            worst = std::max(worst, std::abs(blended[i] - blendedPortable[i]));
        }
    }
    CHECK(worst <= 4.0e-6f);
}
