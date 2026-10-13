#include "game/netplay/RollbackInputHistory.h"

#include <algorithm>
#include <limits>

#include "game/netplay/InputTimeline.h"

namespace gdl::game {
namespace {
bool same(const InputCommand& first, const InputCommand& second) {
    return first.epoch == second.epoch && first.tick == second.tick &&
           first.grant == second.grant && first.seat == second.seat &&
           first.direction == second.direction && first.magnitude == second.magnitude &&
           first.aimDirection == second.aimDirection && first.heldButtons == second.heldButtons &&
           first.pressedButtons == second.pressedButtons;
}
} // namespace

bool RollbackInputHistory::begin(u64 epoch, const Grants& grants) {
    if (epoch == 0 || std::ranges::all_of(grants, [](u32 grant) { return grant == 0; })) {
        return false;
    }
    clear();
    m_epoch = epoch;
    m_grants = grants;
    return true;
}
void RollbackInputHistory::clear() {
    *this = RollbackInputHistory{};
}
bool RollbackInputHistory::record(std::span<const InputCommand> commands, u64 sampled) {
    if (commands.empty() || commands.size() > m_seats.size() || m_epoch == 0) {
        return false;
    }
    std::array<bool, InputCommand::kSeats> seen{};
    for (const auto& input : commands) {
        if (!input.valid() || input.epoch != m_epoch || input.tick != commands.front().tick ||
            input.grant != m_grants[input.seat] || seen[input.seat] ||
            (m_resolved && input.tick <= *m_resolved)) {
            return false;
        }
        seen[input.seat] = true;
        const auto& seat = m_seats[input.seat];
        if (seat.exhausted) {
            return false;
        }
        if (const auto found = seat.commands.find(input.tick);
            found != seat.commands.end() && !same(found->second.input, input)) {
            return false;
        }
    }
    bool overflow = false;
    for (const auto& input : commands) {
        auto& seat = m_seats[input.seat];
        const u64 first =
            seat.commands.empty() ? input.tick : std::min(input.tick, seat.commands.begin()->first);
        const u64 last = seat.commands.empty()
                             ? input.tick
                             : std::max(input.tick, seat.commands.rbegin()->first);
        if ((m_resolved && last - *m_resolved > kHistory) || last - first >= kHistory) {
            seat.exhausted = true;
            seat.replay.clear();
            overflow = true;
        }
    }
    if (overflow) {
        return false;
    }
    for (const auto& input : commands) {
        auto& seat = m_seats[input.seat];
        seat.commands.try_emplace(input.tick, Entry{input, sampled, false});
        seat.replay.clear();
    }
    return true;
}
bool RollbackInputHistory::resolve(u64 epoch, u64 postStepTick) {
    if (epoch != m_epoch || epoch == 0 || (m_resolved && postStepTick <= *m_resolved)) {
        return false;
    }
    m_resolved = postStepTick;
    for (auto& seat : m_seats) {
        seat.commands.erase(seat.commands.begin(), seat.commands.upper_bound(postStepTick));
        seat.replay.clear();
        if (!seat.commands.empty() && seat.commands.rbegin()->first - postStepTick > kHistory) {
            seat.exhausted = true;
        }
    }
    return true;
}
void RollbackInputHistory::invalidate(usize seat) {
    if (seat < m_seats.size()) {
        m_seats[seat] = {};
    }
}
std::span<const RollbackInputHistory::Entry>
RollbackInputHistory::replay(usize index, const std::optional<InputCommand>& previous) {
    if (index >= m_seats.size()) {
        return {};
    }
    auto& seat = m_seats[index];
    seat.replay.clear();
    if (!m_resolved || seat.exhausted || seat.commands.empty() || m_grants[index] == 0) {
        return {};
    }
    if (previous && (!previous->valid() || previous->epoch != m_epoch || previous->seat != index ||
                     previous->grant != m_grants[index] || previous->tick > *m_resolved)) {
        seat.exhausted = true;
        return {};
    }
    const u64 last = seat.commands.rbegin()->first;
    if (last <= *m_resolved || last - *m_resolved > kHistory ||
        *m_resolved == std::numeric_limits<u64>::max()) {
        seat.exhausted = true;
        return {};
    }
    auto held = previous;
    for (u64 offset = 1; offset <= last - *m_resolved; ++offset) {
        const u64 tick = *m_resolved + offset;
        const auto found = seat.commands.find(tick);
        if (found != seat.commands.end()) {
            seat.replay.push_back(found->second);
            held = found->second.input;
        } else {
            seat.replay.push_back(
                {InputTimeline::heldOrNeutral(held, m_epoch, tick, m_grants[index],
                                              static_cast<u8>(index)),
                 0, true});
        }
    }
    return seat.replay;
}
usize RollbackInputHistory::count(usize seat) const {
    return seat < m_seats.size() ? m_seats[seat].commands.size() : 0;
}
usize RollbackInputHistory::pending() const {
    usize result = 0;
    for (const auto& seat : m_seats) {
        result += seat.commands.size();
    }
    return result;
}
bool RollbackInputHistory::resyncRequired(usize seat) const {
    return seat < m_seats.size() && m_seats[seat].exhausted;
}
bool RollbackInputHistory::resyncRequired() const {
    return std::ranges::any_of(m_seats, &Seat::exhausted);
}
} // namespace gdl::game
