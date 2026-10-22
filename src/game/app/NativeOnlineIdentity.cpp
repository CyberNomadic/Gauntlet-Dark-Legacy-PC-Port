#include "game/app/NativeOnlineIdentity.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cfenv>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/Types.h"
#include "engine/io/Sha256.h"
#include "engine/math/Math.h"

#include "game/app/SourceIdentity.h"

#if defined(_WIN32) && defined(_M_X64)
#include <cfloat>
#endif
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace gdl::game {
namespace {

class Digest {
public:
    void word(u64 value) {
        std::array<u8, 8> bytes{};
        for (usize i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<u8>(value >> (8 * i));
        }
        m_hash.update(bytes);
    }
    void text(std::string_view value) {
        word(value.size());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the bytes of the text
        m_hash.update({reinterpret_cast<const u8*>(value.data()), value.size()});
    }
    std::string finish() const { return m_hash.hex(); }

private:
    Sha256 m_hash;
};

void fail(std::string* error, std::string_view message) {
    if (error != nullptr) {
        *error = message;
    }
}

bool digestText(std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool languageText(std::string_view value) {
    return !value.empty() && value.size() <= 32 &&
           std::ranges::all_of(value, [](char c) { return c >= 32 && c <= 126; });
}

// Only what the arithmetic depends on: no operating system, compiler or library
// names, which say who built the game rather than what its numbers are.
void platform(Digest& digest) {
#if !defined(_WIN32) && !defined(__linux__)
    throw std::runtime_error("native online numeric identity is unavailable on this OS");
#endif
#if defined(_M_X64) || defined(__x86_64__)
    digest.text("x86-64");
#else
    throw std::runtime_error("native online numeric identity is unavailable on this architecture");
#endif
    static_assert(std::numeric_limits<f32>::is_iec559);
    static_assert(std::numeric_limits<f64>::is_iec559);
    digest.word(sizeof(void*));
    digest.word(std::numeric_limits<f32>::digits);
    digest.word(std::numeric_limits<f64>::digits);
    digest.word(std::endian::native == std::endian::little ? 1 : 2);
}

#if defined(_M_X64) || defined(__x86_64__)
/** The SSE control bits the witness assumes: round to nearest, gradual underflow,
 * every exception masked. Sticky status flags are excluded. */
u32 sseControl() {
    const u32 sse = _mm_getcsr();
    if (std::fegetround() != FE_TONEAREST || (sse & 0x6000U) != 0) {
        throw std::runtime_error("native online numeric profile requires round-to-nearest");
    }
    if ((sse & 0x8040U) != 0) {
        throw std::runtime_error("native online numeric profile requires gradual underflow");
    }
    if ((sse & 0x1f80U) != 0x1f80U) {
        throw std::runtime_error("native online numeric profile requires masked exceptions");
    }
    return sse & 0xffc0U;
}
#endif

#if defined(_WIN32) && defined(_M_X64)
void windowsControl() {
    unsigned int control = 0;
    if (_controlfp_s(&control, 0, 0) != 0) {
        throw std::runtime_error("cannot inspect native online floating-point control");
    }
    if ((control & _MCW_RC) != _RC_NEAR) {
        throw std::runtime_error("native online numeric profile requires round-to-nearest");
    }
    if ((control & _MCW_DN) != _DN_SAVE) {
        throw std::runtime_error("native online numeric profile requires gradual underflow");
    }
    if ((control & _MCW_EM) != _MCW_EM) {
        throw std::runtime_error("native online numeric profile requires masked exceptions");
    }
    if (_get_FMA3_enable() != 0) {
        throw std::runtime_error("native online numeric profile requires startup FMA3 disable");
    }
}
#endif

class PreserveArithmeticStatus {
public:
    PreserveArithmeticStatus() : m_errno(errno) {
        if (std::fegetenv(&m_environment) != 0) {
            throw std::runtime_error("cannot preserve native online arithmetic status");
        }
    }
    ~PreserveArithmeticStatus() {
        std::fesetenv(&m_environment);
        errno = m_errno;
    }
    PreserveArithmeticStatus(const PreserveArithmeticStatus&) = delete;
    PreserveArithmeticStatus& operator=(const PreserveArithmeticStatus&) = delete;

private:
    int m_errno;
    std::fenv_t m_environment{};
};

// Volatile loads keep the witness as actual operations in this executable,
// rather than compiler-evaluated constants. Every emitted result is hashed by
// its IEEE bits, including signed zero; there is no tolerance or quantization.
f32 operand(u32 bits) {
    const volatile u32 stored = bits;
    return std::bit_cast<f32>(static_cast<u32>(stored));
}

f64 operand(u64 bits) {
    const volatile u64 stored = bits;
    return std::bit_cast<f64>(static_cast<u64>(stored));
}

void result(Digest& digest, f32 value) {
    if (!std::isfinite(value)) {
        throw std::runtime_error("native numeric corpus produced a nonfinite result");
    }
    digest.word(std::bit_cast<u32>(value));
}

void result(Digest& digest, f64 value) {
    if (!std::isfinite(value)) {
        throw std::runtime_error("native numeric corpus produced a nonfinite result");
    }
    digest.word(std::bit_cast<u64>(value));
}

/** A fixed sequence of operands over the ranges the simulation feeds its math,
 * laundered through memory so the compiler cannot fold the operations. */
class Operands {
public:
    f32 single(f32 low, f32 high) {
        const f32 amount = static_cast<f32>(next() >> 8) * (1.0f / 16777216.0f);
        const volatile f32 stored = low + (high - low) * amount;
        return stored;
    }
    f64 wide(f64 low, f64 high) {
        const f64 amount = static_cast<f64>(next()) * (1.0 / 4294967296.0);
        const volatile f64 stored = low + (high - low) * amount;
        return stored;
    }

private:
    u32 next() {
        m_state = m_state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<u32>(m_state >> 32);
    }
    u64 m_state = 0x9E3779B97F4A7C15ULL;
};

void corpus(Digest& digest) {
    const PreserveArithmeticStatus restore;
    digest.text("gdl-native-operation-corpus-v2");
    // Player headings/movement, tree rotations, damping and collision algebra.
    // Include angle boundaries, adjacent unit values and gradual underflow.
    constexpr auto kSingles = std::to_array<u32>(
        {0x00000000U, 0x80000000U, 0x00000001U, 0x00800000U, 0x3eaaaaabU, 0xbeaaaaabU, 0x3f7fffffU,
         0x3f800000U, 0x3f800001U, 0xbf800001U, 0x3fc90fdaU, 0x3fc90fdbU, 0x3fc90fdcU, 0x40490fdaU,
         0x40490fdbU, 0xc0490fdbU, 0x40c90fdbU, 0x447a0001U, 0xc47a0001U});
    for (const auto bits : kSingles) {
        const f32 value = operand(bits);
        const f32 positive = std::abs(value) + operand(u32{0x3e000000});
        result(digest, std::sin(value));
        result(digest, std::cos(value));
        result(digest, std::tan(value));
        result(digest, std::atan2(value, operand(u32{0xbf000001})));
        result(digest, std::asin(std::clamp(value, -1.0f, 1.0f)));
        result(digest, std::acos(std::clamp(value, -1.0f, 1.0f)));
        result(digest, std::sqrt(std::abs(value)));
        result(digest, std::hypot(value, operand(u32{0x3f000001})));
        result(digest, std::pow(positive, operand(u32{0x3f555555})));
        result(digest, std::fmod(value, operand(u32{0x3fc90fdb})));
        result(digest, std::exp(std::clamp(value, -8.0f, 8.0f)));
        result(digest, std::log(positive));
        result(digest, value * operand(u32{0x3f000000}));
        result(digest, value / operand(u32{0x40400000}));
        result(digest, (value + positive) - positive);
    }
    constexpr auto kDoubles = std::to_array<u64>(
        {0x0000000000000000ULL, 0x8000000000000000ULL, 0x0000000000000001ULL, 0x0010000000000000ULL,
         0x3fd5555555555555ULL, 0xbfd5555555555555ULL, 0x3fefffffffffffffULL, 0x3ff0000000000000ULL,
         0x3ff0000000000001ULL, 0xbff0000000000001ULL, 0x3ff921fb54442d17ULL, 0x3ff921fb54442d18ULL,
         0x3ff921fb54442d19ULL, 0x400921fb54442d18ULL, 0xc00921fb54442d18ULL,
         0x408f400000000001ULL});
    for (const auto bits : kDoubles) {
        const f64 value = operand(bits);
        const f64 positive = std::abs(value) + operand(u64{0x3fc0000000000000ULL});
        result(digest, std::sin(value));
        result(digest, std::cos(value));
        result(digest, std::tan(value));
        result(digest, std::atan2(value, operand(u64{0xbfe0000000000001ULL})));
        result(digest, std::asin(std::clamp(value, -1.0, 1.0)));
        result(digest, std::acos(std::clamp(value, -1.0, 1.0)));
        result(digest, std::sqrt(std::abs(value)));
        result(digest, std::hypot(value, operand(u64{0x3fe0000000000001ULL})));
        result(digest, std::pow(positive, operand(u64{0x3feaaaaaaaaaaaabULL})));
        result(digest, std::fmod(value, operand(u64{0x3ff921fb54442d18ULL})));
        result(digest, std::exp(std::clamp(value, -8.0, 8.0)));
        result(digest, std::log(positive));
        result(digest, value * operand(u64{0x3fe0000000000000ULL}));
        result(digest, value / operand(u64{0x4008000000000000ULL}));
        result(digest, (value + positive) - positive);
    }
    // Whether the compiler fuses a multiply with the add that follows it.
    const f32 a = operand(u32{0x3f800001});
    const f32 b = operand(u32{0x3f7ffffe});
    const f32 c = operand(u32{0xbf800000});
    const volatile f32 product = a * b;
    result(digest, a * b + c);
    result(digest, static_cast<f32>(product) + c);
    const f64 da = operand(u64{0x3ff0000000000001ULL});
    const f64 db = operand(u64{0x3feffffffffffffeULL});
    const f64 dc = operand(u64{0xbff0000000000000ULL});
    const volatile f64 doubleProduct = da * db;
    result(digest, da * db + dc);
    result(digest, static_cast<f64>(doubleProduct) + dc);
    // The ranges the simulation feeds its math, headings to blend amounts, over a
    // fixed sequence long enough that a library rounding a few inputs differently
    // from another is seen: that is where platforms part today.
    Operands operands;
    for (u32 sample = 0; sample < 512; ++sample) {
        const f32 angle = operands.single(-4.0f * kPi, 4.0f * kPi);
        const f32 unit = operands.single(-1.0f, 1.0f);
        const f32 length = operands.single(0.001f, 10000.0f);
        const f32 exponent = operands.single(-20.0f, 20.0f);
        const f32 other = operands.single(-100.0f, 100.0f);
        const f32 blend = operands.single(0.0f, 1.0f);
        const f32 power = operands.single(-3.0f, 3.0f);
        result(digest, std::sin(angle));
        result(digest, std::cos(angle));
        result(digest, std::tan(angle));
        result(digest, std::atan2(other, angle));
        result(digest, std::atan(other));
        result(digest, std::asin(unit));
        result(digest, std::acos(unit));
        result(digest, std::sqrt(length));
        result(digest, std::hypot(other, angle));
        result(digest, std::pow(length, power));
        result(digest, std::fmod(other, kTwoPi));
        result(digest, std::exp(exponent));
        result(digest, std::log(length));
        result(digest, std::lerp(other, angle, blend));
    }
    for (u32 sample = 0; sample < 512; ++sample) {
        const f64 angle = operands.wide(-4.0 * std::numbers::pi, 4.0 * std::numbers::pi);
        const f64 unit = operands.wide(-1.0, 1.0);
        const f64 length = operands.wide(0.001, 10000.0);
        const f64 exponent = operands.wide(-20.0, 20.0);
        const f64 other = operands.wide(-100.0, 100.0);
        const f64 blend = operands.wide(0.0, 1.0);
        const f64 power = operands.wide(-3.0, 3.0);
        result(digest, std::sin(angle));
        result(digest, std::cos(angle));
        result(digest, std::tan(angle));
        result(digest, std::atan2(other, angle));
        result(digest, std::atan(other));
        result(digest, std::asin(unit));
        result(digest, std::acos(unit));
        result(digest, std::sqrt(length));
        result(digest, std::hypot(other, angle));
        result(digest, std::pow(length, power));
        result(digest, std::fmod(other, 2.0 * std::numbers::pi));
        result(digest, std::exp(exponent));
        result(digest, std::log(length));
        result(digest, std::lerp(other, angle, blend));
    }

    for (const auto bits : {u32{0x3eaaaaab}, u32{0xbf800001}, u32{0x40490fdb}}) {
        const f32 angle = operand(bits);
        const Vec3 from{angle, a, b};
        const Vec3 to{b, angle, a};
        const Vec3 axis = glm::normalize(from);
        const Vec3 cross = glm::cross(from, to);
        const Quat rotation = glm::angleAxis(angle, axis);
        const Quat blended = glm::slerp(rotation, glm::angleAxis(-angle, glm::normalize(to)),
                                        operand(u32{0x3eaaaaab}));
        const Mat4 matrix = glm::scale(glm::translate(Mat4{1.0f}, from) * glm::mat4_cast(rotation),
                                       Vec3{a, b, operand(u32{0x3f000001})});
        const Mat4 inverse = glm::inverse(matrix);
        const Vec4 point = inverse * (matrix * Vec4{to, 1.0f});
        for (s32 i = 0; i < 3; ++i) {
            result(digest, axis[i]);
            result(digest, cross[i]);
        }
        result(digest, glm::dot(from, to));
        for (s32 i = 0; i < 4; ++i) {
            result(digest, blended[i]);
            result(digest, point[i]);
            for (s32 j = 0; j < 4; ++j) {
                result(digest, matrix[i][j]);
                result(digest, inverse[i][j]);
            }
        }
    }
}

/** The arithmetic as it behaves in this process right now. */
std::string numeric() {
    Digest digest;
    digest.text("gdl-native-numeric-witness-v3");
    platform(digest);
#if defined(_M_X64) || defined(__x86_64__)
    digest.word(sseControl());
#endif
#if defined(_WIN32) && defined(_M_X64)
    windowsControl();
#endif
    corpus(digest);
    return digest.finish();
}

} // namespace

std::string_view NativeOnlineIdentity::runtimeProfile() {
#if defined(_WIN32) && defined(_MSC_VER) && defined(_MT) && !defined(_DLL)
    return "windows-msvc-static-crt-v1";
#elif defined(_WIN32) && defined(_MSC_VER) && defined(_DLL)
    return "windows-msvc-dynamic-crt-v1";
#elif defined(_WIN32)
    return "windows-loaded-runtime-v1";
#elif defined(__linux__)
    return "linux-loaded-runtime-v1";
#else
    return "unsupported-runtime";
#endif
}

bool NativeOnlineIdentity::prepareProcessNumeric(std::string* error) {
    fail(error, {});
    try {
#if defined(_M_X64) || defined(__x86_64__)
        sseControl();
#endif
#if defined(_WIN32) && defined(_M_X64)
        // UCRT enables FMA3 transcendental implementations at startup according
        // to CPU support; their rounding can differ. Establish one path before
        // any worker starts. Never reset caller-owned modes or silently repair
        // a changed FMA policy during a later room entry.
        static std::once_flag prepared;
        std::call_once(prepared, [] {
            if (_set_FMA3_enable(0) != 0) {
                throw std::runtime_error("cannot disable native runtime FMA3 math");
            }
        });
        windowsControl();
#endif
        return true;
    } catch (const std::exception& exception) {
        fail(error, exception.what());
        return false;
    }
}

std::optional<NativeOnlineIdentity::Inputs>
NativeOnlineIdentity::snapshot(const StringTable& strings, std::string* error) {
    fail(error, {});
    if (!strings.loaded() || !languageText(strings.language())) {
        fail(error, "native online identity requires a loaded effective language table");
        return std::nullopt;
    }
    try {
        return Inputs{strings.digest(), strings.language(), numeric()};
    } catch (const std::exception& exception) {
        fail(error, exception.what());
        return std::nullopt;
    }
}

std::optional<NativeOnlineIdentity::Result> NativeOnlineIdentity::capture(Inputs inputs,
                                                                          std::string* error) {
    fail(error, {});
    if (!digestText(inputs.data) || !digestText(inputs.numeric) || !languageText(inputs.language)) {
        fail(error, "native online identity input is incomplete or malformed");
        return std::nullopt;
    }
    if (!digestText(sourceIdentity())) {
        fail(error, "the build carries no source identity");
        return std::nullopt;
    }
    Digest digest;
    digest.text("gdl-native-runtime-identity-v3");
    digest.text("numeric");
    digest.text(inputs.numeric);
    digest.text("sources");
    digest.text(sourceIdentity());
    return Result{digest.finish(), std::move(inputs.data), std::move(inputs.language)};
}

bool NativeOnlineIdentity::numericMatches(const Inputs& inputs) {
    if (!digestText(inputs.numeric)) {
        return false;
    }
    try {
        return inputs.numeric == numeric();
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace gdl::game
