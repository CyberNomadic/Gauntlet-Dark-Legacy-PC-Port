#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

#include "engine/io/Sha256.h"

namespace gdl {
/** Buffered, allocation-free sink for canonical values. A digest checks agreement,
 * not authenticity; callers define and version the field schema. */
class CanonicalDigestSink {
public:
    using Digest = std::array<u8, 32>;
    CanonicalDigestSink() noexcept = default;
    CanonicalDigestSink(CanonicalDigestSink&&) noexcept = default;
    CanonicalDigestSink& operator=(CanonicalDigestSink&&) noexcept = default;
    ~CanonicalDigestSink() = default;
    CanonicalDigestSink(const CanonicalDigestSink& other)
        : m_hash(other.m_hash), m_used(other.m_used), m_size(other.m_size) {
        std::copy_n(other.m_buffer.begin(), m_used, m_buffer.begin());
    }
    CanonicalDigestSink& operator=(const CanonicalDigestSink& other) {
        if (this != &other) {
            m_hash = other.m_hash;
            m_used = other.m_used;
            m_size = other.m_size;
            std::copy_n(other.m_buffer.begin(), m_used, m_buffer.begin());
        }
        return *this;
    }
    void reserve(usize /*unused*/) {}
    usize size() const { return m_size; }
    /** Canonical scalar bytes, with one capacity check rather than one per byte. */
    void word(u32 value) { littleEndian(value); }
    void wide(u64 value) { littleEndian(value); }
    void push_back(u8 value) { // NOLINT(readability-identifier-naming): std::back_inserter
        ++m_size;
        m_buffer[m_used++] = value;
        if (m_used == m_buffer.size()) {
            m_hash.update(m_buffer);
            m_used = 0;
        }
    }
    void append(std::span<const u8> bytes) {
        m_size += bytes.size();
        if (m_used != 0) {
            const usize count = std::min(bytes.size(), m_buffer.size() - m_used);
            std::copy_n(bytes.begin(), count,
                        m_buffer.begin() + static_cast<std::ptrdiff_t>(m_used));
            m_used += count;
            bytes = bytes.subspan(count);
            if (m_used != m_buffer.size()) {
                return;
            }
            m_hash.update(m_buffer);
            m_used = 0;
        }
        const usize full = bytes.size() - bytes.size() % m_buffer.size();
        if (full != 0) {
            m_hash.update(bytes.first(full));
            bytes = bytes.subspan(full);
        }
        std::copy(bytes.begin(), bytes.end(), m_buffer.begin());
        m_used = bytes.size();
    }
    Digest finish() const {
        auto hash = m_hash;
        hash.update(std::span<const u8>{m_buffer}.first(m_used));
        return hash.finish();
    }

private:
    template <class UInt> void littleEndian(UInt value) {
        constexpr usize kCount = sizeof(UInt);
        if (m_buffer.size() - m_used < kCount) {
            std::array<u8, kCount> bytes{};
            for (usize index = 0; index < kCount; ++index) {
                bytes[index] = static_cast<u8>(value >> (index * 8));
            }
            append(bytes);
            return;
        }
        for (usize index = 0; index < kCount; ++index) {
            m_buffer[m_used + index] = static_cast<u8>(value >> (index * 8));
        }
        m_used += kCount;
        m_size += kCount;
        if (m_used == m_buffer.size()) {
            m_hash.update(m_buffer);
            m_used = 0;
        }
    }
    Sha256 m_hash;
    // Only [0,m_used) is initialized and ever read or copied. Short unordered
    // element hashes need not clear four kilobytes of unused scratch storage.
    std::array<u8, 4096> m_buffer{};
    usize m_used = 0;
    usize m_size = 0;
};
} // namespace gdl
