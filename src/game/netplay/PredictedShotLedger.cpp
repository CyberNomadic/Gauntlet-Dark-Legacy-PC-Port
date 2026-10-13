#include "game/netplay/PredictedShotLedger.h"

#include <algorithm>
#include <limits>
#include <set>

namespace gdl::game {
bool PredictedShotLedger::begin(u64 epoch, const Grants& grants) {
    if (epoch == 0) {
        return false;
    }
    if (epoch != m_epoch) {
        clear();
        m_epoch = epoch;
    }
    std::erase_if(m_entries, [&](const auto& pair) {
        return grants[pair.first.seat] != m_grants[pair.first.seat];
    });
    m_grants = grants;
    return !m_failed;
}

void PredictedShotLedger::clear() {
    m_entries.clear();
    m_grants = {};
    m_epoch = 0;
    m_nextInstance = kPrivateInstance;
    m_resolved.reset();
    m_displayed.reset();
    m_failed = false;
}

bool PredictedShotLedger::owned(const ShotIdentity& origin) const {
    return origin.valid() && origin.epoch == m_epoch && m_grants[origin.seat] == origin.grant;
}

bool PredictedShotLedger::admit(usize additions) {
    if (additions > kCapacity - m_entries.size() ||
        additions > std::numeric_limits<u64>::max() - m_nextInstance) {
        // Resuming requires an explicit clear/fresh authoritative reinitialization.
        // Never evict unresolved identities and silently create duplicate shots.
        m_entries.clear();
        m_failed = true;
        return false;
    }
    return true;
}

void PredictedShotLedger::retire(const ShotIdentity& origin, Entry& entry) {
    if (entry.status != Status::Retired) {
        entry.status = Status::Retired;
        entry.retiredAt = std::max(origin.tick, m_resolved.value_or(0));
    }
}

bool PredictedShotLedger::observe(u64 epoch, u64 postStepTick,
                                  std::span<const ShotIdentity> liveOrigins) {
    if (m_failed || m_epoch == 0 || epoch != m_epoch ||
        (m_resolved && postStepTick <= *m_resolved) || liveOrigins.size() > kCapacity) {
        return false;
    }
    std::set<ShotIdentity> live;
    for (const auto& origin : liveOrigins) {
        if (!origin.valid() || origin.epoch != epoch || origin.tick > postStepTick) {
            return false;
        }
        if (owned(origin)) {
            live.insert(origin);
        }
    }
    prune();
    const auto additions = static_cast<usize>(std::ranges::count_if(
        live, [&](const auto& origin) { return !m_entries.contains(origin); }));
    if (!admit(additions)) {
        return false;
    }
    m_resolved = postStepTick;
    for (auto& [origin, entry] : m_entries) {
        if (!live.contains(origin) &&
            (entry.status == Status::Confirmed || origin.tick <= postStepTick)) {
            retire(origin, entry);
        }
    }
    for (const auto& origin : live) {
        auto [found, added] = m_entries.try_emplace(origin);
        auto& entry = found->second;
        if (added) {
            entry.instance = m_nextInstance++;
        }
        // A current host roster always wins over speculative cancellation.
        entry.status = Status::Confirmed;
        entry.retiredAt.reset();
    }
    return true;
}

bool PredictedShotLedger::replace(u8 seat, u64 postStepTick, u64 endTick,
                                  std::span<const ShotIdentity> replayReleases) {
    if (m_failed || m_epoch == 0 || seat >= m_grants.size() || m_grants[seat] == 0 || !m_resolved ||
        postStepTick != *m_resolved || endTick < postStepTick ||
        replayReleases.size() > kCapacity) {
        return false;
    }
    if (endTick - postStepTick > kHorizon) {
        m_entries.clear();
        m_failed = true;
        return false;
    }
    std::set<ShotIdentity> releases;
    for (const auto& origin : replayReleases) {
        if (!owned(origin) || origin.seat != seat || origin.tick <= postStepTick ||
            origin.tick > endTick || !releases.insert(origin).second) {
            return false;
        }
    }
    prune();
    const auto additions = static_cast<usize>(std::ranges::count_if(
        releases, [&](const auto& origin) { return !m_entries.contains(origin); }));
    if (!admit(additions)) {
        return false;
    }
    for (auto& [origin, entry] : m_entries) {
        if (origin.seat == seat && entry.status == Status::Pending && !releases.contains(origin)) {
            retire(origin, entry);
        }
    }
    for (const auto& origin : releases) {
        auto [found, added] = m_entries.try_emplace(origin);
        auto& entry = found->second;
        if (added) {
            entry.instance = m_nextInstance++;
        }
        entry.status = Status::Pending;
        entry.retiredAt.reset();
    }
    return true;
}

void PredictedShotLedger::withdraw(u8 seat) {
    for (auto& [origin, entry] : m_entries) {
        if (origin.seat == seat && entry.status == Status::Pending) {
            retire(origin, entry);
        }
    }
}

void PredictedShotLedger::prune() {
    if (m_displayed) {
        std::erase_if(m_entries, [&](const auto& pair) {
            return pair.second.retiredAt && *pair.second.retiredAt < *m_displayed;
        });
    }
}

bool PredictedShotLedger::advanceDisplayed(u64 epoch, u64 tick) {
    if (m_failed || epoch != m_epoch || m_epoch == 0 || !m_resolved || tick > *m_resolved ||
        (m_displayed && tick < *m_displayed)) {
        return false;
    }
    m_displayed = tick;
    prune();
    return true;
}

bool PredictedShotLedger::suppress(const ShotIdentity& origin) const {
    return !m_failed && owned(origin) && m_entries.contains(origin);
}
} // namespace gdl::game
