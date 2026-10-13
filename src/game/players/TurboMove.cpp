#include "game/players/TurboMove.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/core/Types.h"

#include "game/players/ComboMove.h"
namespace gdl::game {
namespace {
constexpr f32 kMoveNamedFrame = 1.0f;

/** The strong-attack row a free move plays: the strong throw, the power swings, the spin. */
s32 freeMoveOf(PlayerAnimator::Action action, const ClassMoves& moves) {
    switch (action) {
    case PlayerAnimator::Action::StrongThrow: return moves.turboAThrow;
    case PlayerAnimator::Action::PowerClose: return moves.turboAClose;
    case PlayerAnimator::Action::PowerLow: return moves.turboALow;
    case PlayerAnimator::Action::PowerMed: return moves.turboAStep;
    case PlayerAnimator::Action::Spin: return moves.turboA360;
    default: return -1;
    }
}

bool isFreeMove(PlayerAnimator::Action action) {
    return action == PlayerAnimator::Action::StrongThrow ||
           action == PlayerAnimator::Action::PowerClose ||
           action == PlayerAnimator::Action::PowerLow ||
           action == PlayerAnimator::Action::PowerMed || action == PlayerAnimator::Action::Spin;
}
} // namespace

TurboMove::Checkpoint TurboMove::checkpoint() const {
    return {m_pending, m_all, m_owed, m_named, m_weaponHidden, m_volleysShot};
}

u32 TurboMove::Volley::emissionIdentity() const {
    if (slot >= kMaxCheckpointRows || index < 0 || index >= kMaxCheckpointVolleys) {
        return 0;
    }
    return (static_cast<u32>(slot + 1) << 16U) | static_cast<u32>(index);
}

bool TurboMove::valid(const Checkpoint& state) {
    if (state.pending.size() > kMaxCheckpointRows || state.all.size() > kMaxCheckpointRows ||
        state.volleysShot.size() > kMaxCheckpointRows || !std::isfinite(state.owed) ||
        (state.owed != 0 && state.owed != TurboMeter::kStrongCost &&
         state.owed != TurboMeter::kFullCost) ||
        std::ranges::any_of(state.all, [](s32 row) { return row < 0; }) ||
        std::ranges::any_of(state.volleysShot,
                            [](s32 count) { return count < 0 || count > kMaxCheckpointVolleys; })) {
        return false;
    }
    if (state.all.empty()) {
        // Interruption leaves the named latch and old volley counters intact;
        // they are harmless until begin() replaces the entire timeline.
        return state.pending.empty() && state.owed == 0 && !state.weaponHidden;
    }
    if (state.volleysShot.size() != state.all.size()) {
        return false;
    }
    usize next = 0;
    for (const s32 pending : state.pending) {
        while (next < state.all.size() && state.all[next] != pending) {
            ++next;
        }
        if (next == state.all.size()) {
            return false;
        }
        ++next;
        // Both full-turbo chains can contain the same row. A native frame
        // consumes every occurrence together, without sorting or deduplication.
        if (std::ranges::count(state.pending, pending) != std::ranges::count(state.all, pending)) {
            return false;
        }
    }
    return true;
}

bool TurboMove::restore(const Checkpoint& state, const ClassStats* stats) {
    if (!valid(state)) {
        return false;
    }
    if (!state.all.empty()) {
        if (stats == nullptr) {
            return false;
        }
        for (usize slot = 0; slot < state.all.size(); ++slot) {
            const auto index = static_cast<usize>(state.all[slot]);
            if (index >= stats->moveStrikes.size() ||
                (stats->moveStrikes[index].type != MoveStrike::kVolley &&
                 state.volleysShot[slot] != 0)) {
                return false;
            }
        }
    }
    // Copy before replacing any field so a failed allocation cannot leave a
    // mixture of old and new pending strikes.
    Checkpoint restored = state;
    m_pending = std::move(restored.pending);
    m_all = std::move(restored.all);
    m_owed = restored.owed;
    m_named = restored.named;
    m_weaponHidden = restored.weaponHidden;
    m_volleysShot = std::move(restored.volleysShot);
    return true;
}

bool TurboMove::restore(const Checkpoint& state, const ClassStats* stats,
                        PlayerAnimator::Action action) {
    if (!valid(state) || (PlayerAnimator::rollbackShoveAction(action) &&
                          (state.named || !state.volleysShot.empty()))) {
        // A charge always begins a fresh, rowless timeline. Unlike a canceled
        // action's Ready checkpoint it cannot retain old naming/volley residue.
        return false;
    }
    TurboMove expected;
    TurboMeter meter;
    expected.begin(action, stats, meter);
    if (state.all != expected.m_all || (state.owed != 0 && state.owed != expected.m_owed)) {
        return false;
    }
    return restore(state, stats);
}

std::string_view TurboMove::begin(PlayerAnimator::Action action, const ClassStats* known,
                                  TurboMeter& meter) {
    m_pending.clear();
    m_all.clear();
    m_owed = 0.0f;
    m_named = false;
    const bool full = action == PlayerAnimator::Action::TurboFull;
    m_weaponHidden = false;
    m_volleysShot.clear();
    if (isFreeMove(action)) {
        // The strong throw, the power swings and the spin cost nothing; their rows play only
        // while their own action does.
        if (known != nullptr) {
            m_pending = known->strikesOf(freeMoveOf(action, known->moves));
            m_all = m_pending;
            m_volleysShot.assign(m_all.size(), 0);
        }
        return {};
    }
    if (action == PlayerAnimator::Action::ComboAct1) {
        // The two-player combo runs its class's combo rows through COMBOACT1 and is paid
        // for as the partner is taken hold of (pmotion.c 2953), whatever its rows do.
        meter.spend(ComboMove::kCost);
        if (known != nullptr) {
            m_pending = known->strikesOf(known->moves.combo1);
            m_all = m_pending;
            m_volleysShot.assign(m_all.size(), 0);
        }
        return {};
    }
    if (!full && action != PlayerAnimator::Action::TurboStrong) {
        return {};
    }
    m_owed = full ? TurboMeter::kFullCost : TurboMeter::kStrongCost;
    if (const ClassStats* stats = known) {
        if (full) {
            m_pending = stats->strikesOf(stats->moves.turboC1);
            const std::vector<s32> second = stats->strikesOf(stats->moves.turboC2);
            m_pending.insert(m_pending.end(), second.begin(), second.end());
        } else {
            m_pending = stats->strikesOf(stats->moves.turboB);
        }
    }
    m_all = m_pending;
    m_volleysShot.assign(m_all.size(), 0);
    if (m_pending.empty()) {
        meter.spend(m_owed);
        m_owed = 0.0f;
        return full ? "TURBOC" : "TURBOB";
    }
    return {};
}

void TurboMove::advance(PlayerAnimator::Action action, f32 frame, const Vec3& facing,
                        const ClassStats* stats, TurboMeter& meter, const Events& events) {
    const bool attacking = action == PlayerAnimator::Action::TurboFull ||
                           action == PlayerAnimator::Action::TurboStrong || isFreeMove(action) ||
                           action == PlayerAnimator::Action::ComboAct1;
    if (!attacking) {
        m_pending.clear();
        m_all.clear();
        m_owed = 0.0f;
        m_weaponHidden = false;
        return;
    }
    if (stats == nullptr) {
        return;
    }
    // A frame in, the move is named.
    if (!m_named && frame >= kMoveNamedFrame) {
        m_named = true;
        for (const s32 strike : m_all) {
            if (const s32 help = stats->moveStrikes[static_cast<usize>(strike)].help; help >= 0) {
                events.announce(help);
                break;
            }
        }
    }
    // The level goes dark for as long as a strike that darkens it lasts; the hand is empty
    // for as long as one that hides the weapon does; a volley lets fly as it lasts.
    m_weaponHidden = false;
    for (usize slot = 0; slot < m_all.size(); ++slot) {
        const MoveStrike& row = stats->moveStrikes[static_cast<usize>(m_all[slot])];
        if (!row.lasting(frame)) {
            continue;
        }
        if (row.dimming() != 0.0f) {
            events.dim(row.dimming());
        }
        if ((row.flags & MoveStrike::kHidesWeapon) != 0) {
            m_weaponHidden = true;
        }
        if (row.type == MoveStrike::kVolley) {
            volley(slot, row, frame, facing, events);
        }
    }
    std::vector<Strike> due;
    usize nextSlot = 0;
    std::erase_if(m_pending, [&](s32 strike) {
        // Pending rows are a stable subsequence of m_all. Earlier frames may
        // consume nonadjacent rows; searching by the remaining occurrence order
        // also distinguishes repeated authored rows in the two full-turbo chains.
        while (nextSlot < m_all.size() && m_all[nextSlot] != strike) {
            ++nextSlot;
        }
        const usize slot = nextSlot++;
        const bool now =
            frame >= static_cast<f32>(stats->moveStrikes[static_cast<usize>(strike)].startFrame);
        if (now) {
            due.push_back({strike, slot});
        }
        return now;
    });
    for (const auto& strike : due) {
        if (stats->moveStrikes[static_cast<usize>(strike.row)].amount != 0.0f && m_owed > 0.0f) {
            meter.spend(m_owed);
            m_owed = 0.0f;
        }
        if (events.identifiedStrike) {
            events.identifiedStrike(strike);
        } else {
            events.strike(strike.row);
        }
    }
}

void TurboMove::volley(usize slot, const MoveStrike& strike, f32 frame, const Vec3& facing,
                       const Events& events) {
    if (slot >= m_volleysShot.size()) {
        return;
    }
    const f32 since = frame - static_cast<f32>(strike.startFrame);
    const f32 every = strike.delay > 0.0f ? strike.delay : 1.0e6f; // none: one shot only
    const auto due = static_cast<s32>(std::floor(since / every)) + 1;
    const auto span = static_cast<f32>(strike.endFrame - strike.startFrame);
    while (m_volleysShot[slot] < due) {
        const f32 shotAt = static_cast<f32>(m_volleysShot[slot]) * every;
        f32 angle = strike.angle;
        if ((strike.flags & (MoveStrike::kSweepsIn | MoveStrike::kSweepsOut)) != 0) {
            f32 through = span > 0.0f ? std::clamp(shotAt / span, 0.0f, 1.0f) : 1.0f;
            if ((strike.flags & MoveStrike::kSweepsIn) != 0) {
                through = 1.0f - through;
            }
            angle *= through;
        }
        const f32 heading = std::atan2(facing.x, facing.z) + angle;
        const Vec3 direction{std::sin(heading), 0.0f, std::cos(heading)};
        if (events.identifiedVolley) {
            events.identifiedVolley({direction, slot, m_volleysShot[slot]});
        } else {
            events.volley(direction);
        }
        ++m_volleysShot[slot];
    }
}
} // namespace gdl::game
