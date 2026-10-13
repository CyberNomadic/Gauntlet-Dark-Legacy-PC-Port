#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "fixtures/MeasuredPacketTransport.h"

using namespace gdl;

namespace {
class RecordingTransport final : public PacketTransport {
public:
    SendResult result = SendResult::Sent;
    Connection connection = 0;
    Delivery delivery = Delivery::Reliable;
    std::vector<u8> bytes;
    std::vector<Event> events;
    bool closed = false;

    SendResult send(Connection id, std::span<const u8> payload, Delivery mode) override {
        connection = id;
        bytes.assign(payload.begin(), payload.end());
        delivery = mode;
        return result;
    }
    std::vector<Event> poll() override { return std::exchange(events, {}); }
    std::optional<Statistics> statistics(Connection id) const override {
        return id == connection ? std::optional{Statistics{17, 3, 12, true}} : std::nullopt;
    }
    void close(Connection id) override {
        connection = id;
        closed = true;
    }
};
} // namespace

TEST_CASE("network measurements count accepted payload rather than rejected send attempts",
          "[netplay][measured-transport]") {
    RecordingTransport provider;
    test::MeasuredPacketTransport measured(provider);
    const std::array<u8, 3> bytes{1, 2, 3};
    CHECK(measured.send(7, bytes, PacketTransport::Delivery::Unreliable) ==
          PacketTransport::SendResult::Sent);
    CHECK(provider.connection == 7);
    CHECK(provider.bytes == std::vector<u8>{1, 2, 3});
    CHECK(provider.delivery == PacketTransport::Delivery::Unreliable);
    CHECK(measured.send(7, bytes, PacketTransport::Delivery::Reliable) ==
          PacketTransport::SendResult::Sent);
    for (const auto result :
         {PacketTransport::SendResult::Congested, PacketTransport::SendResult::Disconnected,
          PacketTransport::SendResult::Invalid}) {
        provider.result = result;
        CHECK(measured.send(7, bytes, PacketTransport::Delivery::Reliable) == result);
    }
    CHECK(measured.counters().sentBytes == 6);
    CHECK(measured.counters().sentPackets == 2);
    CHECK(measured.counters().deliveryBytes == std::array<u64, 2>{3, 3});
    CHECK(measured.counters().sendResults == std::array<u64, 4>{2, 1, 1, 1});
    const auto stats = measured.statistics(7);
    REQUIRE(stats);
    CHECK(stats->pingMs == 17);
    CHECK(stats->pendingBytes == 3);
    CHECK(stats->queueMicroseconds == 12);
    CHECK(stats->relayed);
    CHECK_FALSE(measured.statistics(6));
    measured.close(7);
    CHECK(provider.closed);
}

TEST_CASE("network measurement reset excludes setup and retains provider events intact",
          "[netplay][measured-transport]") {
    RecordingTransport provider;
    test::MeasuredPacketTransport measured(provider);
    provider.events = {{PacketTransport::EventType::Connected, 1, {}, {}},
                       {PacketTransport::EventType::Message, 1, {4, 5}, {}},
                       {PacketTransport::EventType::Disconnected, 2, {}, "closed"}};
    const auto events = measured.poll();
    REQUIRE(events.size() == 3);
    CHECK(events[1].bytes == std::vector<u8>{4, 5});
    CHECK(events[2].reason == "closed");
    CHECK(measured.counters().receivedBytes == 2);
    CHECK(measured.counters().receivedPackets == 1);
    measured.reset();
    CHECK(measured.counters() == test::MeasuredPacketTransport::Counters{});
    CHECK(measured.poll().empty());
    provider.events = {{PacketTransport::EventType::Message, 1, {6}, {}},
                       {PacketTransport::EventType::Message, 3, {7, 8, 9}, {}}};
    CHECK(measured.poll().size() == 2);
    CHECK(measured.counters().receivedBytes == 4);
    CHECK(measured.counters().receivedPackets == 2);
}
