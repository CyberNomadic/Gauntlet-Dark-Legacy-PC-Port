#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>

#include "engine/net/SessionTransport.h"

#include "game/netplay/MatchSession.h"

namespace gdl::test {
/** Two-to-four-process local iroh test driver. No mocks, snapshots, replica scenes, or
 * public relay. All processes load their own PlayScene and use production
 * MatchSession native-mode load barriers and input routing, without snapshots.
 * The private ticket authenticates this test link; production room admission is
 * deliberately not replaced by this fixed single-seat-per-process harness. */
class NativeProbeLink {
public:
    NativeProbeLink(bool host, const std::filesystem::path& ticket, std::string contract, u8 delay,
                    u8 peers = 2, u8 localPeer = 0);
    std::vector<game::InputCommand> frame(const std::function<game::InputCommand(u64, u8)>& sample);
    void frameCompleted(std::optional<game::NativeAgreement::Digest> observation = std::nullopt);
    void finish();

private:
    using Clock = std::chrono::steady_clock;
    struct Endpoint {
        u8 peer = 0;
        bool helloSent = false;
        bool ready = false;
        bool doneSent = false;
        bool done = false;
        bool disconnected = false;
        u64 completed = 0;
    };
    void pump();
    void openMatch();
    bool every(bool Endpoint::*field) const;
    bool sendControl(PacketTransport::Connection connection, u8 kind);
    bool m_host;
    u8 m_peers;
    u8 m_local;
    std::string m_contract;
    std::filesystem::path m_ticket;
    std::unique_ptr<SessionTransport> m_wire;
    std::map<PacketTransport::Connection, Endpoint> m_endpoints;
    std::unique_ptr<game::MatchSession> m_match = std::make_unique<game::MatchSession>();
    u8 m_delay;
    bool m_invited = false;
    bool m_loaded = false;
    bool m_finishing = false;
    u64 m_completed = 0;
    Clock::time_point m_due;
    Clock::time_point m_pumped = Clock::now();
    std::map<u64, Clock::time_point> m_samples;
    std::optional<u64> m_frameTick;
    std::optional<Clock::time_point> m_frameSample;
    std::vector<double> m_applyMilliseconds;
    double m_waitMilliseconds = 0;
};
} // namespace gdl::test
