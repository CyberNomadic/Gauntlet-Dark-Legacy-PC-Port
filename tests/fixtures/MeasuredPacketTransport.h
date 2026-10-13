#pragma once

#include <array>

#include "engine/net/PacketTransport.h"

namespace gdl::test {
/** Counts application payload accepted by the real provider. Does not invent
 * network delay, count refused sends as traffic, or include QUIC/IP overhead.
 * Reset at gameplay admission to exclude room setup and level loading. */
class MeasuredPacketTransport final : public PacketTransport {
public:
    struct Counters {
        u64 sentBytes = 0;
        u64 sentPackets = 0;
        u64 receivedBytes = 0;
        u64 receivedPackets = 0;
        std::array<u64, 2> deliveryBytes{};
        std::array<u64, 4> sendResults{};
        bool operator==(const Counters&) const = default;
    };
    explicit MeasuredPacketTransport(PacketTransport& provider) : m_provider(provider) {}
    SendResult send(Connection connection, std::span<const u8> bytes, Delivery delivery) override {
        const auto result = m_provider.send(connection, bytes, delivery);
        ++m_counters.sendResults[static_cast<usize>(result)];
        if (result == SendResult::Sent) {
            m_counters.sentBytes += bytes.size();
            ++m_counters.sentPackets;
            m_counters.deliveryBytes[static_cast<usize>(delivery)] += bytes.size();
        }
        return result;
    }
    std::vector<Event> poll() override {
        auto events = m_provider.poll();
        for (const auto& event : events) {
            if (event.type == EventType::Message) {
                m_counters.receivedBytes += event.bytes.size();
                ++m_counters.receivedPackets;
            }
        }
        return events;
    }
    std::optional<Statistics> statistics(Connection connection) const override {
        return m_provider.statistics(connection);
    }
    void close(Connection connection) override { m_provider.close(connection); }
    const Counters& counters() const { return m_counters; }
    void reset() { m_counters = {}; }

private:
    PacketTransport& m_provider;
    Counters m_counters;
};
} // namespace gdl::test
