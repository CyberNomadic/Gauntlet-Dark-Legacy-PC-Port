#include "game/screens/PotionSimulation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "game/combat/DamageTypes.h"
#include "game/players/PowerupEffects.h"

namespace gdl::game {
namespace {
bool validReaction(PlayerDeed deed) {
    return deed == PlayerDeed::None || deed == PlayerDeed::Flinch || deed == PlayerDeed::Reel ||
           deed == PlayerDeed::Spike || deed == PlayerDeed::Die;
}

bool potion(PlayerAnimator::Action action) {
    return action >= PlayerAnimator::Action::UsePotion &&
           action <= PlayerAnimator::Action::ThrowPotionRelease;
}

bool supported(const PlayerAnimator::Checkpoint& state) {
    const auto action = state.action;
    return (PlayerAnimator::rollbackAction(action) || potion(action) ||
            PlayerAnimator::rollbackReactionAction(action) ||
            action == PlayerAnimator::Action::Death) &&
           action != PlayerAnimator::Action::ShieldReady &&
           action != PlayerAnimator::Action::ShieldRun && state.entered && !state.shielded &&
           !state.shieldAsked && !state.potionShielded && !state.legendAsked &&
           !state.legendReleased && !state.superHeld && !state.superReleased &&
           state.itemReleased == PlayerDeed::None && state.comboClass == -1 && !state.comboRide &&
           !state.pushed && state.meleeBlow == MeleeBlow::None;
}

bool vectorWithin(const Vec3& value, f32 limit) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
           std::abs(value.x) <= limit && std::abs(value.y) <= limit && std::abs(value.z) <= limit;
}
} // namespace

bool PotionSimulation::Binding::valid() const {
    return epoch != 0 && grant != 0 && seat < 4;
}

bool PotionSimulation::Context::valid() const {
    if ((damageFlags & ~damage::kHeal) != 0 || (nextKind && (*nextKind < 1 || *nextKind > 4)) ||
        bonuses[0] != 1 || !std::isfinite(powers[0]) ||
        powers[0] < PowerupEffects::kLeastMagicPower ||
        powers[0] > PowerupEffects::kMostMagicPower) {
        return false;
    }
    usize ownColours = 0;
    for (usize kind = 0; kind < powers.size(); ++kind) {
        if ((bonuses[kind] != 1 && bonuses[kind] != damage::kOwnColourBonus) ||
            !std::isfinite(powers[kind]) || powers[kind] != powers[0] * bonuses[kind]) {
            return false;
        }
        ownColours += bonuses[kind] != 1 ? 1U : 0U;
    }
    return ownColours <= 1;
}

bool PotionSimulation::Checkpoint::valid() const {
    const bool transitioning =
        animation.animation.transitionTime < animation.animation.transitionLength;
    return animation.valid() && supported(animation) &&
           transitioning == transitionPose.has_value() &&
           (!transitionPose || transitionPose->valid()) &&
           potions.size() <= static_cast<usize>(Inventory::kMostPotions) &&
           std::ranges::all_of(potions, [](s32 kind) { return kind >= 0 && kind <= 4; }) &&
           context.valid() && validReaction(reaction) && remaining <= kMaxTicks;
}

bool PotionSimulation::Identity::valid() const {
    return owner.valid() && generation != 0 && kind <= Kind::Throw;
}

bool PotionSimulation::restore(const TreeInfo& tree, const Binding& binding,
                               const Checkpoint& state) {
    if (!binding.valid() || !state.valid()) {
        return false;
    }
    PlayerAnimator animator;
    if (!animator.bind(tree, false) || !animator.restore(state.animation, state.transitionPose)) {
        return false;
    }
    m_binding = binding;
    m_animator = std::move(animator);
    m_potions = state.potions;
    m_context = state.context;
    m_reaction = state.reaction;
    m_tick = state.tick;
    m_remaining = state.remaining;
    m_releases.clear();
    return true;
}

void PotionSimulation::clear() {
    m_binding = {};
    m_animator.unbind();
    m_potions.clear();
    m_context = {};
    m_reaction = PlayerDeed::None;
    m_tick = 0;
    m_remaining = 0;
    m_releases.clear();
}

PotionSimulation::Checkpoint PotionSimulation::checkpoint() const {
    return {m_tick,
            m_animator.checkpoint(),
            m_animator.transitionPose(),
            m_potions,
            m_context,
            m_reaction,
            m_remaining};
}

bool PotionSimulation::step(const Frame& frame) {
    if (!m_animator.bound() || !m_binding.valid() || m_remaining == 0 ||
        m_tick == std::numeric_limits<u64>::max() || frame.tick != m_tick + 1 ||
        frame.motion > PlayerMotion::Run || frame.strafe > StrafeWay::Right ||
        !vectorWithin(frame.position, 1'000'000) || !vectorWithin(frame.facing, 1) ||
        frame.facing.y != 0 || std::abs(glm::dot(frame.facing, frame.facing) - 1.0f) > 1e-5f) {
        return false;
    }
    const bool asked = frame.deed == PlayerDeed::UsePotion || frame.deed == PlayerDeed::ThrowPotion;
    if ((m_reaction != PlayerDeed::None && frame.deed != m_reaction) ||
        (m_reaction == PlayerDeed::None && frame.deed != PlayerDeed::None && !asked) ||
        ((m_animator.reacting() || m_animator.dying()) && asked) || (asked && m_potions.empty())) {
        return false;
    }
    // Unknown shared kind-zero state cannot be invented from this seat's stack.
    // A known cancellation may still terminate its unspent wind-up safely.
    const bool mayRelease = asked || m_animator.conjuring();
    if (m_reaction == PlayerDeed::None && mayRelease && !m_potions.empty() &&
        m_potions.back() == 0 && !m_context.nextKind) {
        return false;
    }
    PlayerAnimator next = m_animator;
    next.setStrafe(frame.strafe);
    next.setAttackSpeed(m_context.rapid, m_context.speed);
    next.update(frame.motion, 1, kStep, frame.deed);
    if (!next.checkpoint().valid() || !supported(next.checkpoint()) || next.released() ||
        next.strongReleased() || next.superReleased() || next.meleeStruck() || next.turboBegan()) {
        return false;
    }
    std::optional<Release> emitted;
    auto context = m_context;
    if ((next.potionUsed() || next.potionThrown()) && !m_potions.empty()) {
        const s32 stored = m_potions.back();
        s32 resolved = stored;
        if (stored == 0) {
            if (!context.nextKind) {
                return false;
            }
            resolved = *context.nextKind;
            context.nextKind = static_cast<u8>(resolved % 4 + 1);
        }
        const auto kind = next.potionThrown() ? Kind::Throw : Kind::Use;
        const Identity identity{m_binding, frame.tick, next.player().generation(), kind};
        if (!identity.valid() || m_releases.size() >= static_cast<usize>(Inventory::kMostPotions)) {
            return false;
        }
        const PotionEmission::Context native{m_binding.seat,
                                             frame.position,
                                             frame.facing,
                                             resolved,
                                             context.powers[static_cast<usize>(stored)],
                                             context.bonuses[static_cast<usize>(stored)],
                                             context.damageFlags};
        if (kind == Kind::Throw) {
            emitted = Release{identity, stored, native,
                              PotionEmission::throwPotion(native, next.potionThrowTicks())};
        } else {
            emitted = Release{identity, stored, native, PotionEmission::burst(native)};
        }
    }
    m_animator = std::move(next);
    m_context = context;
    if (emitted) {
        m_potions.pop_back();
        m_releases.push_back(std::move(*emitted));
    }
    m_reaction = PlayerDeed::None;
    m_tick = frame.tick;
    --m_remaining;
    return true;
}
} // namespace gdl::game
