#include "game/netplay/LockstepInputs.h"

#include <algorithm>
#include <limits>
#include <set>

namespace gdl::game {
namespace {
bool same(const InputCommand& a, const InputCommand& b) {
    // Wire encoding, rather than float equality, catches conflicting signed-zero
    // inputs too. Only valid commands reach this point; there is no struct padding.
    return InputPacket::encode(std::span{&a, 1}) == InputPacket::encode(std::span{&b, 1});
}
} // namespace

bool LockstepInputs::occupied(u8 peer) const {
    return peer != 0 && std::ranges::find(m_owners, peer) != m_owners.end();
}
bool LockstepInputs::open(u8 localPeer, u64 epoch, const Owners& owners, const Grants& grants,
                          u8 delay, std::span<const Link> links) {
    if (m_open || localPeer == 0 || localPeer > 4 || epoch == 0 || delay > kMaxDelay ||
        std::ranges::find(owners, localPeer) == owners.end() ||
        std::ranges::find(owners, 1) == owners.end()) {
        return false;
    }
    for (usize seat = 0; seat < owners.size(); ++seat) {
        if (owners[seat] > 4 || (owners[seat] == 0) != (grants[seat] == 0)) {
            return false;
        }
    }
    std::array<PacketTransport::Connection, 5> connections{};
    std::set<PacketTransport::Connection> unique;
    for (const auto& link : links) {
        if (link.peer == 0 || link.peer > 4 || link.peer == localPeer || link.connection == 0 ||
            connections[link.peer] != 0 || !unique.insert(link.connection).second ||
            std::ranges::find(owners, link.peer) == owners.end() ||
            (localPeer != 1 && link.peer != 1)) {
            return false;
        }
        connections[link.peer] = link.connection;
    }
    for (u8 peer = 1; peer <= 4; ++peer) {
        const bool needed = peer != localPeer && std::ranges::find(owners, peer) != owners.end() &&
                            (localPeer == 1 || peer == 1);
        if ((connections[peer] != 0) != needed) {
            return false;
        }
    }
    m_open = true;
    m_local = localPeer;
    m_delay = delay;
    m_epoch = epoch;
    m_owners = owners;
    m_grants = grants;
    m_links = connections;
    m_sample = 1 + delay;
    // Exactly the same neutral lead-in on every peer. It requires no packets and
    // does not authorize concealment after these explicitly agreed startup ticks.
    for (u64 tick = 1; tick <= delay; ++tick) {
        auto& pending = m_pending[tick];
        for (u8 seat = 0; seat < owners.size(); ++seat) {
            if (owners[seat] != 0) {
                InputCommand command;
                command.epoch = epoch;
                command.tick = tick;
                command.grant = grants[seat];
                command.seat = seat;
                pending[seat] = command;
            }
        }
    }
    return true;
}
bool LockstepInputs::queueReady(u8 source) const {
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (m_links[peer] != 0 && peer != source && m_outgoing[peer].size() >= kQueuedPackets) {
            return false;
        }
    }
    return true;
}
void LockstepInputs::queue(u8 source, std::span<const InputCommand> commands) {
    const auto bytes = InputPacket::encode(commands);
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (m_links[peer] != 0 && peer != source) {
            m_outgoing[peer].push_back(*bytes);
        }
    }
}
LockstepInputs::Admission LockstepInputs::admit(u8 peer, std::span<const InputCommand> commands) {
    if (!m_open || m_failed || !occupied(peer) || commands.empty() ||
        commands.size() > InputCommand::kSeats) {
        return Admission::Invalid;
    }
    const u64 tick = commands.front().tick;
    std::array<bool, InputCommand::kSeats> seen{};
    for (const auto& command : commands) {
        if (!command.valid() || command.tick != tick || command.epoch != m_epoch ||
            command.grant != m_grants[command.seat] || seen[command.seat]) {
            return Admission::Invalid;
        }
        if (m_owners[command.seat] != peer) {
            return Admission::WrongPeer;
        }
        seen[command.seat] = true;
    }
    for (usize seat = 0; seat < seen.size(); ++seat) {
        if (seen[seat] != (m_owners[seat] == peer)) {
            return Admission::Invalid;
        }
    }
    if (tick < m_tick) {
        const auto previous = m_history.find(tick);
        if (previous == m_history.end()) {
            return Admission::Stale;
        }
        return std::ranges::all_of(commands,
                                   [&](const auto& command) {
                                       return same(command, previous->second[command.seat]);
                                   })
                   ? Admission::Duplicate
                   : Admission::Conflict;
    }
    if (tick - m_tick >= kWindow) {
        return Admission::TooEarly;
    }
    const auto found = m_pending.find(tick);
    bool duplicate = true;
    for (const auto& command : commands) {
        if (found != m_pending.end() && found->second[command.seat]) {
            if (!same(command, *found->second[command.seat])) {
                return Admission::Conflict;
            }
        } else {
            duplicate = false;
        }
    }
    if (duplicate) {
        return Admission::Duplicate;
    }
    if ((peer == m_local || m_local == 1) && !queueReady(peer)) {
        return Admission::Full;
    }
    for (const auto& command : commands) {
        m_pending[tick][command.seat] = command;
    }
    // Guests store their own input immediately. Host relays each guest's exact
    // packet to the other guests; no host simulation/update is required to relay.
    if (peer == m_local || m_local == 1) {
        queue(peer, commands);
    }
    return Admission::Accepted;
}
bool LockstepInputs::sampleReady() const {
    return m_open && !m_failed && m_tick <= std::numeric_limits<u64>::max() - m_delay &&
           m_sample <= m_tick + m_delay && queueReady(m_local);
}
bool LockstepInputs::sample(std::span<const InputCommand> commands) {
    if (!sampleReady() || commands.empty() || commands.front().tick != m_sample ||
        m_sample == std::numeric_limits<u64>::max() ||
        admit(m_local, commands) != Admission::Accepted) {
        return false;
    }
    ++m_sample;
    return true;
}
LockstepInputs::Admission LockstepInputs::receive(PacketTransport::Connection connection,
                                                  std::span<const u8> bytes) {
    if (!m_open || m_failed || connection == 0) {
        return Admission::WrongPeer;
    }
    const auto source = std::ranges::find(m_links, connection);
    if (source == m_links.end()) {
        return Admission::WrongPeer;
    }
    const auto commands = InputPacket::decode(bytes);
    if (!commands || commands->empty()) {
        return Admission::Invalid;
    }
    const u8 owner = m_owners[commands->front().seat];
    const u8 sender = static_cast<u8>(source - m_links.begin());
    if (owner == m_local || (m_local == 1 && owner != sender)) {
        return Admission::WrongPeer;
    }
    return admit(owner, *commands);
}
bool LockstepInputs::ready() const {
    if (!m_open || m_failed || m_tick == std::numeric_limits<u64>::max()) {
        return false;
    }
    const auto found = m_pending.find(m_tick);
    if (found == m_pending.end()) {
        return false;
    }
    for (usize seat = 0; seat < m_owners.size(); ++seat) {
        if (m_owners[seat] != 0 && !found->second[seat]) {
            return false;
        }
    }
    return true;
}
std::optional<LockstepInputs::Frame> LockstepInputs::advance() {
    if (!ready()) {
        return std::nullopt;
    }
    Frame frame;
    const auto found = m_pending.find(m_tick);
    for (usize seat = 0; seat < frame.size(); ++seat) {
        if (found->second[seat]) {
            frame[seat] = *found->second[seat];
        }
    }
    m_history.emplace(m_tick, frame);
    if (m_history.size() > kWindow) {
        m_history.erase(m_history.begin());
    }
    m_pending.erase(found);
    ++m_tick;
    return frame;
}
bool LockstepInputs::flush(PacketTransport& transport) {
    if (!m_open || m_failed) {
        return false;
    }
    for (u8 peer = 1; peer <= 4; ++peer) {
        auto& packets = m_outgoing[peer];
        while (!packets.empty()) {
            const auto result =
                transport.send(m_links[peer], packets.front(), PacketTransport::Delivery::Reliable);
            if (result == PacketTransport::SendResult::Congested) {
                break;
            }
            if (result != PacketTransport::SendResult::Sent) {
                m_failed = true;
                return false;
            }
            packets.pop_front();
        }
    }
    return true;
}
void LockstepInputs::disconnected(PacketTransport::Connection connection) {
    if (connection != 0 && std::ranges::find(m_links, connection) != m_links.end()) {
        m_failed = true;
    }
}
usize LockstepInputs::queuedPackets() const {
    usize result = 0;
    for (const auto& queue : m_outgoing) {
        result += queue.size();
    }
    return result;
}
} // namespace gdl::game
