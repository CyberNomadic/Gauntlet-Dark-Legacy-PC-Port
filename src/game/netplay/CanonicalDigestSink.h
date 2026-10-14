#pragma once

#include <algorithm>
#include <array>
#include <span>

#include "engine/io/Sha256.h"

namespace gdl::game {
/** Allocation-free byte sink for the same explicit scalar visitors used on the
 * wire. SHA-256 agreement is probabilistic, not authentication or validation.
 * Buffering avoids a hash update call for every individual scalar byte. */
class CanonicalDigestSink {
public:
    using Digest = std::array<u8, 32>;
    void reserve(usize) {}
    usize size() const { return m_size; }
    void push_back(u8 value) { append(std::span<const u8>{&value, 1}); }
    void append(std::span<const u8> bytes) {
        m_size += bytes.size();
        while (!bytes.empty()) {
            const usize count = std::min(bytes.size(), m_buffer.size() - m_used);
            std::copy_n(bytes.begin(), count, m_buffer.begin() + m_used);
            m_used += count;
            bytes = bytes.subspan(count);
            if (m_used == m_buffer.size()) {
                m_hash.update(m_buffer);
                m_used = 0;
            }
        }
    }
    Digest finish() const {
        auto hash = m_hash;
        hash.update(std::span<const u8>{m_buffer}.first(m_used));
        return hash.finish();
    }

private:
    Sha256 m_hash;
    std::array<u8, 4096> m_buffer{};
    usize m_used = 0;
    usize m_size = 0;
};
} // namespace gdl::game
