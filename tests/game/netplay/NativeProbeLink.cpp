#include "NativeProbeLink.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace gdl::test {
namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
constexpr auto kTimeout = std::chrono::seconds(45);
constexpr auto kTick = std::chrono::nanoseconds(16'666'667);
// The HELLO digest binds the actual native stage/workload. This fixed harness
// catalog has one already-loaded scene, not a path received from the network.
constexpr u32 kScene = 1;
} // namespace

NativeProbeLink::NativeProbeLink(bool host, const std::filesystem::path& ticket,
                                 std::string contract, u8 delay, u8 peers, u8 localPeer)
    : m_host(host), m_peers(peers), m_local(localPeer == 0 ? (host ? 1 : 2) : localPeer),
      m_contract(std::move(contract)), m_ticket(ticket), m_delay(delay) {
    require(m_contract.size() == 64, "Invalid native test contract digest");
    require(peers >= 2 && peers <= 4 && m_local >= 1 && m_local <= peers && (m_local == 1) == host,
            "Invalid native test peer roster");
    std::string invitation;
    if (!host) {
        std::ifstream file(ticket, std::ios::binary);
        require(file.good() && std::filesystem::file_size(ticket) <= 8192,
                "Native test invitation missing or oversized");
        invitation.assign(std::istreambuf_iterator<char>{file}, {});
    } else {
        require(!std::filesystem::exists(ticket), "Refusing an existing native test ticket");
    }
    std::string error;
    m_wire = openSessionTransport({true, invitation}, error);
    require(m_wire != nullptr, "Could not start local native test transport");
    const auto deadline = Clock::now() + kTimeout;
    while (m_match->phase() != game::MatchSession::Phase::Running) {
        require(Clock::now() < deadline, "Native test load barrier timed out");
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    m_due = Clock::now();
}
bool NativeProbeLink::every(bool Endpoint::*field) const {
    return m_endpoints.size() == (m_host ? static_cast<usize>(m_peers - 1) : usize{1}) &&
           std::ranges::all_of(m_endpoints, [&](const auto& entry) { return entry.second.*field; });
}
void NativeProbeLink::openMatch() {
    if (m_match->phase() != game::MatchSession::Phase::Offline || !every(&Endpoint::ready) ||
        !every(&Endpoint::helloSent)) {
        return;
    }
    game::MatchOwners owners{};
    for (u8 seat = 0; seat < m_peers; ++seat) {
        owners[seat] = static_cast<u8>(seat + 1);
    }
    std::vector<game::MatchLink> links;
    for (const auto& [connection, endpoint] : m_endpoints) {
        links.push_back({endpoint.peer, connection});
    }
    require(m_match->open(m_local, owners, links, m_delay), "Could not open native match session");
}
bool NativeProbeLink::sendControl(PacketTransport::Connection connection, u8 kind) {
    std::vector<u8> bytes{'G', 'D', 'L', 'N', kind, m_local, m_peers, 2};
    bytes.insert(bytes.end(), m_contract.begin(), m_contract.end());
    const u64 completed = kind == 1 ? 0 : m_completed;
    for (u32 shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<u8>(completed >> shift));
    }
    const auto result = m_wire->send(connection, bytes, PacketTransport::Delivery::Reliable);
    require(result == PacketTransport::SendResult::Sent ||
                result == PacketTransport::SendResult::Congested,
            "Native test control send failed");
    return result == PacketTransport::SendResult::Sent;
}
void NativeProbeLink::pump() {
    require(m_wire->phase() != SessionTransport::Phase::Failed, "Native test transport failed");
    if (m_host && !m_invited && m_wire->phase() == SessionTransport::Phase::Ready) {
        const auto temporary = std::filesystem::path{m_ticket.string() + ".pending"};
        require(!std::filesystem::exists(temporary), "Native test ticket temporary exists");
        std::ofstream file(temporary, std::ios::binary);
        file << m_wire->invitation();
        file.close();
        require(!file.fail(), "Could not write native test invitation");
        std::filesystem::rename(temporary, m_ticket);
        m_invited = true;
    }
    for (const auto& event : m_wire->poll()) {
        if (event.type == PacketTransport::EventType::Connected) {
            require(event.connection != 0 &&
                        m_endpoints.size() <
                            (m_host ? static_cast<usize>(m_peers - 1) : usize{1}) &&
                        m_endpoints.emplace(event.connection, Endpoint{}).second,
                    "Unexpected native test endpoint");
        } else if (event.type == PacketTransport::EventType::Disconnected) {
            const auto found = m_endpoints.find(event.connection);
            require(found != m_endpoints.end(), "Native test disconnect from unknown endpoint");
            auto& endpoint = found->second;
            if (!(m_finishing && m_host && endpoint.doneSent && endpoint.done &&
                  endpoint.completed == m_completed)) {
                m_match->disconnected(event.connection);
                throw std::runtime_error("Native test peer disconnected before completion");
            }
            // Every scene has finished and received its terminal acknowledgment.
            // Retiring one guest must not stop the other guests' completion barrier.
            endpoint.disconnected = true;
        } else {
            const auto found = m_endpoints.find(event.connection);
            require(found != m_endpoints.end() && !found->second.disconnected,
                    "Native test message from unknown endpoint");
            auto& endpoint = found->second;
            const auto& bytes = event.bytes;
            if (bytes.size() >= 4 && bytes[0] == 'G' && bytes[1] == 'D' && bytes[2] == 'L' &&
                bytes[3] == 'N') {
                require(bytes.size() == 80 && bytes[6] == m_peers && bytes[7] == 2 &&
                            (m_host ? (bytes[5] >= 2 && bytes[5] <= m_peers) : bytes[5] == 1) &&
                            (endpoint.peer == 0 || endpoint.peer == bytes[5]) &&
                            std::equal(m_contract.begin(), m_contract.end(), bytes.begin() + 8),
                        "Native test peers disagree on data or workload");
                for (const auto& [connection, other] : m_endpoints) {
                    require(connection == event.connection || other.peer != bytes[5],
                            "Duplicate native test peer identity");
                }
                u64 completed = 0;
                for (u32 index = 0; index < 8; ++index) {
                    completed |= static_cast<u64>(bytes[72 + index]) << (index * 8);
                }
                if (bytes[4] == 1) {
                    require(completed == 0, "Invalid native test HELLO tick");
                    endpoint.peer = bytes[5];
                    endpoint.ready = true;
                    openMatch();
                } else {
                    require(bytes[4] == 2 && endpoint.ready && m_loaded && completed != 0 &&
                                (!endpoint.done || endpoint.completed == completed) &&
                                (!m_finishing || completed == m_completed) &&
                                m_match->phase() == game::MatchSession::Phase::Running,
                            "Invalid native test completion");
                    endpoint.done = true;
                    endpoint.completed = completed;
                }
            } else {
                require(endpoint.ready, "Native match packet arrived before HELLO");
                openMatch();
                const auto result = m_match->receive(event.connection, bytes);
                require(result == game::MatchSession::Admission::Accepted ||
                            result == game::MatchSession::Admission::Stale,
                        "Native match protocol rejected a packet");
            }
        }
    }
    for (auto& [connection, endpoint] : m_endpoints) {
        if (!endpoint.disconnected && !endpoint.helloSent) {
            endpoint.helloSent = sendControl(connection, 1);
        }
    }
    openMatch();
    if (m_match->phase() != game::MatchSession::Phase::Offline && !every(&Endpoint::disconnected)) {
        if (every(&Endpoint::helloSent) && every(&Endpoint::ready)) {
            if (m_host && m_match->phase() == game::MatchSession::Phase::Lobby) {
                require(m_match->prepare(kScene), "Could not prepare native probe scene");
            }
            if (!m_loaded && m_match->phase() == game::MatchSession::Phase::Loading) {
                const auto& context = m_match->context();
                require(context.scene == kScene && context.tickRate == 60 &&
                            context.transition == game::MatchTransition::Start,
                        "Unexpected native probe scene context");
                // The caller opened its independent PlayScene before constructing
                // this link. HELLO agreement is not a substitute for Ready/Commit.
                require(m_match->loaded(), "Native probe scene readiness rejected");
                m_loaded = true;
            }
        }
        const auto now = Clock::now();
        m_match->update(std::chrono::duration<f64>(now - m_pumped).count(), now);
        m_pumped = now;
        m_match->flush(*m_wire, now);
        require(m_match->phase() != game::MatchSession::Phase::Stopped,
                "Native match session stopped");
        const auto* agreement = m_match->nativeAgreement();
        const bool agreed =
            m_completed < game::MatchSession::kNativeAgreementPeriod ||
            (agreement &&
             agreement->agreedTick() ==
                 m_completed - m_completed % game::MatchSession::kNativeAgreementPeriod);
        if (m_finishing && agreed && (!m_host || every(&Endpoint::done))) {
            for (auto& [connection, endpoint] : m_endpoints) {
                require(!endpoint.done || endpoint.completed == m_completed,
                        "Native test peers completed different ticks");
                if (!endpoint.disconnected && !endpoint.doneSent) {
                    endpoint.doneSent = sendControl(connection, 2);
                }
            }
        }
    }
}
std::vector<game::InputCommand>
NativeProbeLink::frame(const std::function<game::InputCommand(u64, u8)>& sample) {
    require(!m_frameTick && !m_finishing && m_match->phase() == game::MatchSession::Phase::Running,
            "Native probe requested a frame before completing the previous scene tick");
    const auto start = Clock::now();
    const auto deadline = start + kTimeout;
    // Presentation pacing never selects input numbers or gameplay delta time.
    while (Clock::now() < m_due) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool submitted = false;
    const auto sampleLocal = [&] {
        if (!submitted && m_match->inputReady()) {
            const auto sampled = Clock::now();
            const auto localSeat = static_cast<u8>(m_local - 1);
            auto command = sample(m_match->inputTick(), localSeat);
            require(command.seat == localSeat, "Native test journal sampled a foreign seat");
            // The journal supplies semantic controls. Session generations are
            // issued by Prepare, not the journal's standalone fixture epoch.
            command.epoch = m_match->context().epoch;
            command.grant = m_match->context().grants[command.seat];
            require(m_match->sample(std::span{&command, 1}),
                    "Native local input rejected");
            require(m_samples.emplace(command.tick, sampled).second,
                    "Native probe sampled the same input tick twice");
            submitted = true;
            m_match->flush(*m_wire);
            require(m_match->phase() == game::MatchSession::Phase::Running,
                    "Native match input send failed");
        }
    };
    sampleLocal();
    std::optional<game::LockstepInputs::Frame> frame;
    do {
        pump();
        require(Clock::now() < deadline, "Native simulation stalled waiting for peer input");
        // Retry backpressure without inventing another sample or dropping an edge.
        sampleLocal();
        frame = m_match->advance();
        if (!frame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } while (!frame);
    m_frameTick = (*frame)[0].tick;
    const auto found = m_samples.find((*frame)[0].tick);
    m_frameSample.reset();
    if (found != m_samples.end()) {
        m_frameSample = found->second;
        m_samples.erase(found);
    }
    m_waitMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    m_due = std::max(m_due + kTick, Clock::now() - 3 * kTick);
    return {frame->begin(), frame->begin() + m_peers};
}
void NativeProbeLink::frameCompleted(std::optional<game::NativeAgreement::Digest> observation) {
    require(m_frameTick && m_match->completedNativeTick(*m_frameTick),
            "Native scene completion did not match the issued input tick");
    m_completed = *m_frameTick;
    m_frameTick.reset();
    if (m_completed % game::MatchSession::kNativeAgreementPeriod == 0) {
        require(observation && m_match->observeNative(m_completed, *observation),
                "Native simulation observation refused");
    } else {
        require(!observation, "Unexpected off-period native observation");
    }
    if (m_frameSample) {
        m_applyMilliseconds.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() - *m_frameSample).count());
        m_frameSample.reset();
    }
}
void NativeProbeLink::finish() {
    require(!m_frameTick && m_match->phase() == game::MatchSession::Phase::Running,
            "Native probe finished with an uncompleted scene tick");
    m_finishing = true;
    const auto deadline = Clock::now() + kTimeout;
    while (!(m_host ? every(&Endpoint::disconnected)
                    : (every(&Endpoint::doneSent) && every(&Endpoint::done)))) {
        require(Clock::now() < deadline, "Native test completion barrier timed out");
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::ranges::sort(m_applyMilliseconds);
    const auto percentile = [&](usize percent) {
        return m_applyMilliseconds.empty()
                   ? 0.0
                   : m_applyMilliseconds[(m_applyMilliseconds.size() - 1) * percent / 100];
    };
    std::cout << "Native network: peer=" << static_cast<unsigned>(m_local)
              << " peers=" << static_cast<unsigned>(m_peers)
              << " input_delay_ticks=" << static_cast<unsigned>(m_delay)
              << " measured_local_samples=" << m_applyMilliseconds.size()
              << " sample_to_native_update_p50_ms=" << percentile(50)
              << " sample_to_native_update_p95_ms=" << percentile(95)
              << " pacing_and_input_wait_ms=" << m_waitMilliseconds << '\n';
    for (const auto& [connection, endpoint] : m_endpoints) {
        if (!endpoint.disconnected) {
            m_wire->close(connection);
        }
    }
}
} // namespace gdl::test
