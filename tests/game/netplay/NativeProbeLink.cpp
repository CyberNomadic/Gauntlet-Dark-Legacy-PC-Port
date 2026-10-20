#include "NativeProbeLink.h"

#include <algorithm>
#include <array>
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
double percentile(std::vector<double> values, usize percent) {
    if (values.empty()) {
        return 0.0;
    }
    std::ranges::sort(values);
    return values[(values.size() - 1) * percent / 100];
}
} // namespace

/** Only test input datagrams pass through this queue. QUIC/reliable session
 * traffic and the underlying provider's RTT remain completely unmodified. */
class NativeProbeLink::ImpairedTransport final : public SessionTransport {
public:
    ImpairedTransport(std::unique_ptr<SessionTransport> wire, NativeProbeImpairment config, u8 peer)
        : m_wire(std::move(wire)), m_config(config), m_peer(peer),
          m_random(static_cast<u64>(config.seed) ^ (static_cast<u64>(peer) << 32)) {}
    Phase phase() const override { return m_wire->phase(); }
    std::string invitation() const override { return m_wire->invitation(); }
    std::string error() const override { return m_wire->error(); }
    std::optional<Statistics> statistics(Connection connection) const override {
        return m_wire->statistics(connection);
    }
    SendResult send(Connection connection, std::span<const u8> bytes, Delivery delivery) override {
        if (delivery != Delivery::Unreliable || !game::RollbackExchange::recognizes(bytes)) {
            return m_wire->send(connection, bytes, delivery);
        }
        if (!m_config.enabled()) {
            const auto result = m_wire->send(connection, bytes, delivery);
            if (result == SendResult::Sent) {
                ++m_metrics.attempted;
                ++m_metrics.transmitted;
            }
            return result;
        }
        // Apply backpressure before drawing random decisions: retries are not
        // fresh impairment trials and cannot grow an unbounded delayed queue.
        if (m_queue.size() >= kQueueLimit) {
            return SendResult::Congested;
        }
        const auto message = game::RollbackExchange::decode(bytes);
        require(message.has_value(), "Impairment received a malformed native input datagram");
        for (const auto& command : message->commands) {
            m_latestInput[message->origin] = std::max(m_latestInput[message->origin], command.tick);
        }
        const auto now = Clock::now();
        updateOutage(now);
        if (m_peer == m_config.outagePeer && message->origin == m_peer && m_config.outageMs != 0 &&
            m_metrics.outageStartedTick == 0 && message->nextTick >= m_config.outageTick) {
            m_metrics.outageStartedTick = message->nextTick;
            m_outageStart = now;
            m_outageEnd = now + std::chrono::milliseconds(m_config.outageMs);
        }
        ++m_metrics.attempted;
        if (m_metrics.outageStartedTick != 0 && !m_metrics.outageFinished) {
            ++m_metrics.outageDropped;
            return SendResult::Sent;
        }
        if (random() % 1'000'000 < m_config.lossPpm) {
            ++m_metrics.lossDropped;
            return SendResult::Sent;
        }
        const s64 jitter = static_cast<s64>(random() % (2 * m_config.jitterMs + 1)) -
                           static_cast<s64>(m_config.jitterMs);
        s64 delay = std::max<s64>(0, static_cast<s64>(m_config.latencyMs) + jitter);
        if (random() % 1'000'000 < m_config.reorderPpm) {
            delay += m_config.reorderMs;
        }
        m_queue.emplace(now + std::chrono::milliseconds(delay),
                        Queued{connection, std::vector<u8>(bytes.begin(), bytes.end()), now,
                               ++m_serial});
        ++m_metrics.enqueued;
        m_metrics.queuePeak = std::max<u64>(m_metrics.queuePeak, m_queue.size());
        return SendResult::Sent;
    }
    std::vector<Event> poll() override {
        const auto now = Clock::now();
        updateOutage(now);
        usize budget = kReceiveBudget;
        // A burst is a complete outbound application-input blackout, including
        // datagrams already waiting in the latency/reorder queue. They resume
        // afterward with their actual residence; reliable traffic never waits.
        const bool outage = m_metrics.outageStartedTick != 0 && !m_metrics.outageFinished;
        while (!outage && budget-- != 0 && !m_queue.empty() && m_queue.begin()->first <= now) {
            const auto first = m_queue.begin();
            const auto& queued = first->second;
            auto message = game::RollbackExchange::decode(queued.bytes);
            require(message.has_value(), "Queued native datagram no longer decodes");
            const auto residence = std::chrono::duration_cast<std::chrono::microseconds>(
                                       now - queued.enqueued).count();
            // Account for this measured application queue exactly once. The
            // receiver adds real Iroh link transit independently; its RTT is not
            // replaced by a synthetic value. Repeated send attempts use raw bytes.
            message->progressAgeUs += static_cast<u32>(std::min<u64>(
                static_cast<u64>(residence),
                game::RollbackExchange::kMaxProgressAgeUs - message->progressAgeUs));
            const auto bytes = game::RollbackExchange::encode(*message);
            require(bytes.has_value(), "Queued native progress age overflowed");
            const auto sent = m_wire->send(queued.connection, *bytes, Delivery::Unreliable);
            if (sent == SendResult::Congested) {
                break;
            }
            if (sent == SendResult::Disconnected) {
                abandon(queued.connection);
                continue; // poll still delivers the real disconnect to the session barrier.
            }
            require(sent == SendResult::Sent, "Delayed native input datagram send failed");
            auto& last = m_lastSent[queued.connection];
            if (queued.serial < last) {
                ++m_metrics.reordered;
            }
            last = std::max(last, queued.serial);
            ++m_metrics.transmitted;
            if (m_metrics.outageFinished && queued.enqueued >= m_outageEnd && message->origin == m_peer &&
                std::ranges::any_of(message->commands, [&](const auto& command) {
                    return m_latestInput[message->origin] - command.tick >=
                           game::RollbackInputs::kRedundantTicks;
                })) {
                ++m_metrics.oldestRecoveryDatagrams;
            }
            m_residenceMilliseconds.push_back(static_cast<double>(residence) / 1000.0);
            m_queue.erase(first);
        }
        auto events = m_wire->poll();
        for (const auto& event : events) {
            if (event.type == EventType::Disconnected) {
                abandon(event.connection);
            }
        }
        return events;
    }
    void close(Connection connection) override {
        abandon(connection);
        m_wire->close(connection);
    }
    void observeNativeUpdate(Clock::time_point begin, Clock::time_point end,
                             u64 next, u64 confirmed,
                             const game::NativeRollbackDriver::Statistics& before,
                             const game::NativeRollbackDriver& driver) {
        // Count only forward attempts wholly inside this sender's measured
        // application-input blackout. Lifetime counters cannot show whether a
        // wait happened during the injected outage or an unrelated earlier gap.
        if (m_metrics.outageStartedTick == 0 || begin < m_outageStart || end > m_outageEnd) {
            return;
        }
        auto& progress = m_metrics.outageProgress;
        if (progress.updates == 0) {
            progress.firstNextTick = next;
            progress.firstConfirmedTick = confirmed;
        }
        ++progress.updates;
        progress.lastNextTick = driver.nextTick();
        progress.lastConfirmedTick = driver.confirmedTick();
        progress.stalls += driver.statistics().stalls - before.stalls;
        progress.pacingIdles += driver.statistics().pacingIdles - before.pacingIdles;
        progress.predictionMaxTicks = std::max(
            {progress.predictionMaxTicks, next - 1 - confirmed,
             driver.nextTick() - 1 - driver.confirmedTick()});
    }
    Metrics::Impairment metrics() const {
        auto result = m_metrics;
        result.queuePending = m_queue.size();
        result.residenceP50Ms = percentile(m_residenceMilliseconds, 50);
        result.residenceP95Ms = percentile(m_residenceMilliseconds, 95);
        result.residenceMaxMs = percentile(m_residenceMilliseconds, 100);
        return result;
    }
private:
    static constexpr usize kQueueLimit = 4096;
    struct Queued {
        Connection connection;
        std::vector<u8> bytes;
        Clock::time_point enqueued;
        u64 serial;
    };
    u32 random() {
        // SplitMix64 is only the test scheduler's random source, never native
        // simulation state. Peer-specific streams avoid correlated link loss.
        u64 value = (m_random += 0x9e3779b97f4a7c15ULL);
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return static_cast<u32>(value ^ (value >> 31));
    }
    void updateOutage(Clock::time_point now) {
        if (m_metrics.outageStartedTick != 0 && !m_metrics.outageFinished && now >= m_outageEnd) {
            m_metrics.outageFinished = true;
            m_metrics.outageElapsedMs =
                std::chrono::duration<double, std::milli>(now - m_outageStart).count();
        }
    }
    void abandon(Connection connection) {
        const auto before = m_queue.size();
        std::erase_if(m_queue, [=](const auto& item) { return item.second.connection == connection; });
        m_metrics.queueAbandoned += before - m_queue.size();
    }
    std::unique_ptr<SessionTransport> m_wire;
    NativeProbeImpairment m_config;
    u8 m_peer;
    u64 m_random;
    u64 m_serial = 0;
    Clock::time_point m_outageStart, m_outageEnd;
    std::multimap<Clock::time_point, Queued> m_queue;
    std::map<Connection, u64> m_lastSent;
    std::array<u64, 5> m_latestInput{};
    Metrics::Impairment m_metrics;
    std::vector<double> m_residenceMilliseconds;
};

NativeProbeLink::NativeProbeLink(bool host, const std::filesystem::path& ticket,
                                 std::string contract, u8 delay, game::PlayScene& scene,
                                 u8 peers, u8 localPeer, NativeProbeImpairment impairment, bool relayOnly)
    : m_host(host), m_peers(peers), m_local(localPeer == 0 ? (host ? 1 : 2) : localPeer),
      m_contract(std::move(contract)), m_ticket(ticket), m_scene(scene), m_delay(delay),
      m_relayOnly(relayOnly) {
    require(m_contract.size() == 64, "Invalid native test contract digest");
    require(peers >= 2 && peers <= 4 && m_local >= 1 && m_local <= peers && (m_local == 1) == host,
            "Invalid native test peer roster");
    require(impairment.latencyMs <= 500 && impairment.jitterMs <= 500 &&
                impairment.lossPpm <= 250'000 && impairment.reorderPpm <= 1'000'000 &&
                impairment.reorderMs <= 500 && impairment.seed != 0 &&
                ((impairment.reorderPpm == 0) == (impairment.reorderMs == 0)) &&
                ((impairment.outageMs == 0 && impairment.outageTick == 0 && impairment.outagePeer == 0) ||
                 (impairment.outageMs > 0 && impairment.outageMs <= 5000 && impairment.outageTick > 0 &&
                  impairment.outagePeer >= 1 && impairment.outagePeer <= peers)),
            "Invalid native test impairment configuration");
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
    auto wire = openSessionTransport({!relayOnly, invitation, relayOnly}, error);
    require(wire != nullptr, "Could not start native test transport");
    m_wire = std::make_unique<ImpairedTransport>(std::move(wire), impairment, m_local);
    const auto deadline = Clock::now() + kTimeout;
    while (m_match->phase() != game::MatchSession::Phase::Running) {
        require(Clock::now() < deadline, "Native test load barrier timed out");
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    m_due = Clock::now();
}
NativeProbeLink::~NativeProbeLink() {
    const auto epoch = m_match->context().epoch;
    m_match->detachNativeInputs(epoch);
    m_driver.clear();
    if (m_scene.recordedAudioEpoch() == epoch) {
        m_scene.endRecordedAudio();
    }
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
    links.reserve(m_endpoints.size());
    for (const auto& [connection, endpoint] : m_endpoints) {
        links.push_back({endpoint.peer, connection});
    }
    require(m_match->open(m_local, owners, links, m_delay), "Could not open native match session");
}
bool NativeProbeLink::sendControl(PacketTransport::Connection connection, u8 kind) {
    std::vector<u8> bytes{'G', 'D', 'L', 'N', kind, m_local, m_peers, 4};
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
void NativeProbeLink::pump(bool send) {
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
            if (!m_finishing || !m_host || !endpoint.doneSent || !endpoint.done ||
                endpoint.completed != m_completed) {
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
                require(bytes.size() == 80 && bytes[6] == m_peers && bytes[7] == 4 &&
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
                const auto link = m_wire->statistics(event.connection);
                const auto result = m_match->receive(event.connection, bytes, std::nullopt,
                                                     link ? link->pingMs : -1);
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
                require(m_scene.beginRecordedAudio(context.epoch),
                        "Native probe could not attach recorded audio");
                require(m_driver.bind(m_scene, context, m_local,
                                      game::NativeRollbackDriver::AudioMode::Required),
                        "Native probe rollback binding refused");
                require(m_match->attachNativeInputs(context.epoch,
                    [this](const game::RollbackExchange::Message& message, std::optional<u8> transit) {
                        using Admission = game::RollbackInputs::Admission;
                        if (!message.commands.empty()) {
                            const auto admitted = m_driver.submit(message.origin, message.commands);
                            if (admitted != Admission::Accepted &&
                                admitted != Admission::Duplicate && admitted != Admission::Stale) {
                                return false;
                            }
                        }
                        if (transit) {
                            const auto progress = m_driver.observePeerTick(
                                message.origin, message.epoch, message.nextTick, *transit);
                            if (progress != Admission::Accepted &&
                                progress != Admission::Duplicate && progress != Admission::Stale) {
                                return false;
                            }
                        }
                        return true;
                    }), "Native probe input consumer refused");
                require(m_match->loaded(), "Native probe scene readiness rejected");
                m_loaded = true;
            }
        }
        const auto now = Clock::now();
        const bool retiring = std::ranges::any_of(m_endpoints, [](const auto& entry) {
            return entry.second.disconnected;
        });
        // A disconnect is accepted above only after that guest received the
        // all-peer completion acknowledgment. No gameplay remains to flush;
        // sending retained match packets to its closed link would stop the
        // other guests before they receive their reliable DONE messages.
        if (!retiring) {
            m_match->update(std::chrono::duration<f64>(now - m_pumped).count(), now);
        }
        m_pumped = now;
        if (!retiring && send &&
            (m_match->phase() != game::MatchSession::Phase::Running || now >= m_sendDue)) {
            m_match->flush(*m_wire, now);
            m_sendDue = now + kTick;
        }
        require(m_match->phase() != game::MatchSession::Phase::Stopped,
                "Native match session stopped");
        const auto* agreement = m_match->nativeAgreement();
        const bool agreed =
            m_completed < game::MatchSession::kNativeAgreementPeriod ||
            ((agreement != nullptr) &&
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
void NativeProbeLink::execute(bool forward, Step& step) {
    const auto before = m_driver.statistics();
    const auto next = m_driver.nextTick();
    const auto confirmed = m_driver.confirmedTick();
    const auto start = Clock::now();
    auto result = m_driver.advance(forward);
    const auto ended = Clock::now();
    require(result.status == game::NativeRollbackDriver::Status::Held ||
                result.status == game::NativeRollbackDriver::Status::Advanced,
            "Native rollback failed or reached an unsupported campaign boundary");
    require(!m_driver.tentativeTerminal(), "Native probe reached a speculative campaign boundary");
    if (forward) {
        m_wire->observeNativeUpdate(start, ended, next, confirmed, before, m_driver);
    }
    if (std::ranges::any_of(result.frames, &game::NativeRollbackDriver::ExecutedFrame::replay)) {
        m_correctionMilliseconds.push_back(
            std::chrono::duration<double, std::milli>(ended - start).count());
    }
    for (auto& frame : result.frames) {
        if (!frame.replay) {
            const auto sample = m_samples.find(frame.tick);
            if (sample != m_samples.end()) {
                // First local application, not later confirmation or replay.
                m_applyMilliseconds.push_back(
                    std::chrono::duration<double, std::milli>(ended - sample->second).count());
                m_samples.erase(sample);
            }
            step.advanced = true;
        }
        require(frame.tick > m_reported, "Correction crossed already reported confirmation");
        m_frames.insert_or_assign(frame.tick, std::move(frame));
    }
    require(result.audio.has_value(), "Native probe lost required recorded audio");
    // No SoundPlayer/device exists in this harness. Dispatch still exercises
    // the once-only presentation bookkeeping, strictly after replay returns.
    m_scene.dispatchRecordedAudio(*result.audio);
    require(m_match->publishNativeInputs(m_driver.redundantInputs(), m_driver.nextTick()),
            "Native probe progress publication refused");
    require(m_match->confirmNativeTick(m_match->context().epoch, m_driver.confirmedTick()),
            "Native probe executed confirmation refused");
    m_completed = m_driver.confirmedTick();
    for (const auto& hash : result.digests) {
        require(hash.tick <= m_completed && m_digests.emplace(hash.tick, hash.digest).second &&
                    m_match->observeNative(hash.tick, hash.digest),
                "Native probe confirmed agreement refused");
    }
    while (m_reported < m_completed) {
        const auto tick = ++m_reported;
        const auto found = m_frames.find(tick);
        require(found != m_frames.end(), "Missing actual confirmed native frame");
        require(found->second.audio.has_value(), "Missing confirmed native audio journal");
        std::optional<game::NativeAgreement::Digest> hash;
        if (tick % game::NativeRollbackDriver::kDigestPeriod == 0) {
            const auto saved = m_digests.find(tick);
            require(saved != m_digests.end(), "Missing confirmed native state digest");
            hash = saved->second;
            m_digests.erase(saved);
        }
        step.confirmed.push_back({std::move(found->second), hash});
        m_frames.erase(found);
    }
    require(m_frames.size() <= game::RollbackInputs::kPredictionTicks,
            "Unbounded speculative native report buffer");
    const auto prediction = m_driver.nextTick() - 1 - m_completed;
    require(prediction <= game::RollbackInputs::kPredictionTicks,
            "Native probe exceeded prediction window");
    m_predictionMaxTicks = std::max(m_predictionMaxTicks, prediction);
    step.simulatedThrough = m_driver.nextTick() - 1;
}

NativeProbeLink::Step NativeProbeLink::frame(
    u64 target, const std::function<game::InputCommand(u64, u8)>& sample) {
    require(target != 0 && !m_finishing && m_completed < target &&
                m_match->phase() == game::MatchSession::Phase::Running,
            "Native probe requested a frame outside its running target");
    while (Clock::now() < m_due) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    pump(false);
    observePaths();
    Step result;
    if (m_driver.sampleTick() <= target) {
        const auto sampled = Clock::now();
        const auto tick = m_driver.sampleTick();
        const auto seat = static_cast<u8>(m_local - 1);
        const auto command = sample(tick, seat);
        require(command.valid() && command.seat == seat && command.tick == tick,
                "Native test journal sampled an invalid or foreign input");
        game::SessionInputs::Frame local;
        local[0] = game::SessionInputs::playInput(command);
        require(m_driver.sample(local) &&
                    (m_driver.sampleTick() == tick || m_driver.sampleTick() == tick + 1),
                "Native probe input sample was refused or skipped its capture tick");
        // Match the shipping sample-before-correction order. Backpressure may
        // retain this same sample tick across polls; measure from its first
        // physical capture, not from the later poll that admits it.
        m_samples.try_emplace(tick, sampled);
    }
    require(m_match->publishNativeInputs(m_driver.redundantInputs(), m_driver.nextTick()),
            "Native probe input publication refused");
    execute(m_driver.nextTick() <= target, result);
    m_scene.serviceRecordedAudio(1.0f / 60.0f);
    const auto now = Clock::now();
    m_match->flush(*m_wire, now);
    require(m_match->phase() == game::MatchSession::Phase::Running,
            "Native probe transport or agreement failed");
    m_sendDue = now + kTick;
    m_due = std::max(m_due + kTick, now - 3 * kTick);
    return result;
}
void NativeProbeLink::finish() {
    require(m_reported == m_completed && m_driver.nextTick() == m_completed + 1 &&
                m_frames.empty() && m_digests.empty() &&
                m_match->phase() == game::MatchSession::Phase::Running,
            "Native probe finished with an uncompleted scene tick");
    observePaths();
    require(m_paths.size() == (m_host ? static_cast<usize>(m_peers - 1) : usize{1}),
            "Native test lacks path observations for an active link");
    if (m_relayOnly) {
        for (const auto& [peer, path] : m_paths) {
            (void)peer;
            require(path.relayed != 0 && path.direct == 0,
                    "Requested relay run lacks actual relay evidence for an active link");
        }
    }
    m_finishing = true;
    for (const auto& [connection, endpoint] : m_endpoints) {
        (void)endpoint;
        const auto stats = m_wire->statistics(connection);
        m_rtt.push_back(stats ? stats->pingMs : -1);
    }
    const auto deadline = Clock::now() + kTimeout;
    while (!(m_host ? every(&Endpoint::disconnected)
                    : (every(&Endpoint::doneSent) && every(&Endpoint::done)))) {
        require(Clock::now() < deadline, "Native test completion barrier timed out");
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto measured = metrics();
    std::cout << "Native network: peer=" << static_cast<unsigned>(m_local)
              << " peers=" << static_cast<unsigned>(m_peers)
              << " input_delay_ticks=" << static_cast<unsigned>(m_delay)
              << " measured_local_samples=" << measured.sampledInputs
              << " input_sample_to_first_native_update_p50_ms=" << measured.inputToUpdateP50Ms
              << " input_sample_to_first_native_update_p95_ms=" << measured.inputToUpdateP95Ms
              << " replay_ticks=" << measured.simulation.replayTicks
              << " correction_p95_ms=" << measured.correctionP95Ms << '\n';
    for (const auto& [connection, endpoint] : m_endpoints) {
        if (!endpoint.disconnected) {
            m_wire->close(connection);
        }
    }
}

NativeProbeLink::Metrics NativeProbeLink::metrics() const {
    Metrics result{m_wire->metrics(), m_applyMilliseconds.size(), percentile(m_applyMilliseconds, 50),
            percentile(m_applyMilliseconds, 95), m_driver.statistics(), m_predictionMaxTicks,
            percentile(m_correctionMilliseconds, 50), percentile(m_correctionMilliseconds, 95),
            percentile(m_correctionMilliseconds, 100), m_rtt, m_local, {}};
    for (const auto& [peer, path] : m_paths) {
        result.links.push_back({peer, path.relayed + path.direct + path.unavailable,
                               path.relayed, path.direct, path.unavailable,
                               percentile(path.rtt, 50), percentile(path.rtt, 95),
                               percentile(path.rtt, 100)});
    }
    return result;
}

void NativeProbeLink::observePaths() {
    // Provider RTT is measured below the optional application-datagram delay.
    // Sample every native turn, including stalled turns, not only at shutdown.
    for (const auto& [connection, endpoint] : m_endpoints) {
        if (endpoint.disconnected || !endpoint.ready) {
            continue;
        }
        auto& path = m_paths[endpoint.peer];
        const auto stats = m_wire->statistics(connection);
        if (!stats || stats->pingMs < 0) {
            ++path.unavailable;
            continue;
        }
        if (stats->relayed) {
            ++path.relayed;
        } else {
            ++path.direct;
        }
        require(!m_relayOnly || stats->relayed,
                "Relay-only diagnostic observed a direct transport path");
        path.rtt.push_back(stats->pingMs);
    }
}

game::NativeAgreement::Digest NativeProbeLink::finalDigest() {
    require(m_driver.nextTick() == m_completed + 1, "Final scene is still speculative");
    const auto scene = m_scene.nativeAgreementDigest();
    require(scene.has_value(), "Final native scene digest refused");
    return game::NativeRollbackDriver::agreementDigest(
        *scene, {m_driver.arrivalVotes(), m_driver.tentativeTerminal()});
}
} // namespace gdl::test
