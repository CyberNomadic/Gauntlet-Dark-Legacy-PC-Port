#include "engine/io/Sha256.h"

#include <algorithm>
#include <bit>

#if (defined(_M_X64) || defined(__x86_64__)) && !defined(_M_ARM64EC)
#define GDL_SHA256_X64 1
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#else
#define GDL_SHA256_X64 0
#endif

namespace gdl {
namespace {
#if GDL_SHA256_X64
bool hasSha256Instructions() {
#if defined(_MSC_VER)
    int features[4]{};
    __cpuid(features, 0);
    if (features[0] < 7) return false;
    __cpuidex(features, 7, 0);
    return (static_cast<u32>(features[1]) & (1U << 29)) != 0;
#else
    unsigned int a = 0, b = 0, c = 0, d = 0;
    return __get_cpuid_max(0, nullptr) >= 7 && __get_cpuid_count(7, 0, &a, &b, &c, &d) &&
           (b & (1U << 29)) != 0;
#endif
}

// Only this function may contain SHA extensions. Do not raise the baseline CPU
// requirement for the executable; older x64 and other architectures use C++.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("sha"), noinline))
#elif defined(_MSC_VER)
__declspec(noinline)
#endif
void compressAccelerated(std::array<u32, 8>& state, const std::array<u32, 64>& words,
                         const std::array<u32, 64>& constants) {
    auto abef = _mm_setr_epi32(static_cast<s32>(state[5]), static_cast<s32>(state[4]),
                              static_cast<s32>(state[1]), static_cast<s32>(state[0]));
    auto cdgh = _mm_setr_epi32(static_cast<s32>(state[7]), static_cast<s32>(state[6]),
                              static_cast<s32>(state[3]), static_cast<s32>(state[2]));
    // SHA256RNDS2 returns ABEF after two rounds. The old ABEF is the new
    // CDGH, so the next instruction swaps their roles. Only SSE2 plus the
    // CPUID-checked SHA instructions are used; no AVX/SSSE3/SSE4 dependency.
    // Instruction semantics: Intel SHA Extensions, SHA256RNDS2 input/output table.
    for (usize i = 0; i < words.size(); i += 4) {
        const auto message = _mm_add_epi32(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(words.data() + i)),
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(constants.data() + i)));
        cdgh = _mm_sha256rnds2_epu32(cdgh, abef, message);
        abef = _mm_sha256rnds2_epu32(abef, cdgh, _mm_shuffle_epi32(message, 0x0E));
    }
    std::array<u32, 4> first{}, second{};
    _mm_storeu_si128(reinterpret_cast<__m128i*>(first.data()), abef);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(second.data()), cdgh);
    state[0] += first[3];
    state[1] += first[2];
    state[2] += second[3];
    state[3] += second[2];
    state[4] += first[1];
    state[5] += first[0];
    state[6] += second[1];
    state[7] += second[0];
}
#endif
} // namespace

bool Sha256::hardwareAccelerationAvailable() {
#if GDL_SHA256_X64
    static const bool available = hasSha256Instructions();
    return available;
#else
    return false;
#endif
}

void Sha256::block(std::span<const u8, 64> bytes) {
    static constexpr std::array<u32, 64> kRound{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    std::array<u32, 64> words{};
    for (usize i = 0; i < 16; ++i) {
        for (usize j = 0; j < 4; ++j) {
            words[i] = (words[i] << 8) | bytes[i * 4 + j];
        }
    }
    for (usize i = 16; i < words.size(); ++i) {
        const auto x = words[i - 15];
        const auto y = words[i - 2];
        words[i] = words[i - 16] + (std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3)) + words[i - 7] +
                   (std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10));
    }
#if GDL_SHA256_X64
    if (m_backend == Backend::Automatic && hardwareAccelerationAvailable()) {
        compressAccelerated(m_state, words, kRound);
        return;
    }
#endif
    auto [a, b, c, d, e, f, g, h] = m_state;
    const auto round = [](u32 a, u32 b, u32 c, u32& d, u32 e, u32 f, u32 g, u32& h,
                          u32 constant, u32 word) {
        const u32 t1 = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
                       ((e & f) ^ (~e & g)) + constant + word;
        const u32 t2 =
            (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        d += t1;
        h = t1 + t2;
    };
    // Rotate the argument names rather than moving all eight working words on
    // every round. Eight rounds restore their original roles. This keeps the
    // portable SHA stream identical without selecting CPU-specific instructions.
    for (usize i = 0; i < words.size(); i += 8) {
        round(a, b, c, d, e, f, g, h, kRound[i], words[i]);
        round(h, a, b, c, d, e, f, g, kRound[i + 1], words[i + 1]);
        round(g, h, a, b, c, d, e, f, kRound[i + 2], words[i + 2]);
        round(f, g, h, a, b, c, d, e, kRound[i + 3], words[i + 3]);
        round(e, f, g, h, a, b, c, d, kRound[i + 4], words[i + 4]);
        round(d, e, f, g, h, a, b, c, kRound[i + 5], words[i + 5]);
        round(c, d, e, f, g, h, a, b, kRound[i + 6], words[i + 6]);
        round(b, c, d, e, f, g, h, a, kRound[i + 7], words[i + 7]);
    }
    const std::array<u32, 8> result{a, b, c, d, e, f, g, h};
    for (usize i = 0; i < result.size(); ++i) {
        m_state[i] += result[i];
    }
}
void Sha256::update(std::span<const u8> bytes) {
    m_size += bytes.size();
    if (m_used != 0) {
        const auto count = std::min(bytes.size(), m_buffer.size() - m_used);
        std::copy_n(bytes.begin(), count, m_buffer.begin() + m_used);
        m_used += count;
        bytes = bytes.subspan(count);
        if (m_used != m_buffer.size()) {
            return;
        }
        block(m_buffer);
        m_used = 0;
    }
    // Full blocks can be read directly, including from unaligned input. Only
    // the incomplete tail needs to survive the caller's span.
    while (bytes.size() >= m_buffer.size()) {
        block(bytes.first<64>());
        bytes = bytes.subspan(m_buffer.size());
    }
    std::copy(bytes.begin(), bytes.end(), m_buffer.begin());
    m_used = bytes.size();
}
std::array<u8, 32> Sha256::finish() const {
    auto copy = *this;
    // Finish in one bulk update. A short unordered-element digest otherwise
    // spent dozens of update calls appending individual padding zeroes.
    std::array<u8, 128> padding{};
    padding[0] = 0x80;
    const usize lengthAt = m_used < 56 ? 56 - m_used : 120 - m_used;
    for (usize i = 0; i < 8; ++i) {
        padding[lengthAt + 7 - i] = static_cast<u8>((m_size * 8) >> (i * 8));
    }
    copy.update(std::span<const u8>{padding}.first(lengthAt + 8));
    std::array<u8, 32> result{};
    for (usize i = 0; i < result.size(); ++i) {
        result[i] = static_cast<u8>(copy.m_state[i / 4] >> ((3 - i % 4) * 8));
    }
    return result;
}
std::string Sha256::hex() const {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string result;
    for (const auto byte : finish()) {
        result += kDigits[byte >> 4];
        result += kDigits[byte & 15];
    }
    return result;
}
} // namespace gdl
