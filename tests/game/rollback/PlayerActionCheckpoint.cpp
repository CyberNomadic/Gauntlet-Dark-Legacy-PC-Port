#include "game/rollback/PlayerActionCheckpoint.h"

#include <array>
#include <bit>
#include <utility>

#include "engine/io/ByteReader.h"

#include "game/rollback/AnimationCheckpoint.h"

namespace gdl::game {
namespace {
using State = PlayerAnimator::Checkpoint;
constexpr std::array kFlags{
    &State::entered,     &State::released,    &State::quickPress,     &State::strongPress,
    &State::quickHeld,   &State::strongHeld,  &State::moved,          &State::shielded,
    &State::pushed,      &State::potionUsed,  &State::potionThrown,   &State::potionThrowReleased,
    &State::dead,        &State::turboBegan,  &State::strongReleased, &State::potionShielded,
    &State::shieldAsked, &State::legendAsked, &State::legendReleased, &State::superReleased,
    &State::superHeld,   &State::rapid,       &State::speed,          &State::comboRide,
    &State::potionLatch};
void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
void wide(std::vector<u8>& bytes, u64 value) {
    word(bytes, static_cast<u32>(value));
    word(bytes, static_cast<u32>(value >> 32U));
}
u64 wide(ByteReader& reader) {
    const u64 low = reader.readU32();
    return low | (static_cast<u64>(reader.readU32()) << 32U);
}
void real(std::vector<u8>& bytes, f32 value) {
    word(bytes, std::bit_cast<u32>(value));
}
f32 real(ByteReader& reader) {
    return std::bit_cast<f32>(reader.readU32());
}
} // namespace

std::optional<std::vector<u8>> PlayerActionCheckpoint::encode(const State& state) {
    if (!state.valid()) {
        return std::nullopt;
    }
    auto encodedClock = AnimationCheckpoint::encode(state.animation);
    if (!encodedClock) {
        return std::nullopt;
    }
    auto bytes = std::move(*encodedClock);
    bytes.reserve(kBytes);
    word(bytes, static_cast<u32>(state.action));
    word(bytes, static_cast<u32>(state.footfall));
    word(bytes, static_cast<u32>(state.meleeBlow));
    word(bytes, static_cast<u32>(state.melee.range));
    word(bytes, state.melee.low ? 1U : 0U);
    real(bytes, state.melee.yaw);
    for (const s32 value : {state.character, state.chain, state.potionThrowTicks, state.comboClass,
                            state.stillTicks, state.fidgetTicks}) {
        word(bytes, std::bit_cast<u32>(value));
    }
    word(bytes, static_cast<u32>(state.itemReleased));
    word(bytes, static_cast<u32>(state.strafe));
    real(bytes, state.guardArmor);
    real(bytes, state.attackSeconds);
    wide(bytes, std::bit_cast<u64>(state.phaseSeconds));
    real(bytes, state.shotFrame);
    u32 flags = 0;
    for (usize i = 0; i < kFlags.size(); ++i) {
        if (state.*kFlags[i]) {
            flags |= 1U << i;
        }
    }
    word(bytes, flags);
    return bytes;
}

std::optional<State> PlayerActionCheckpoint::decode(std::span<const u8> bytes) {
    if (bytes.size() != kBytes) {
        return std::nullopt;
    }
    const auto clock = AnimationCheckpoint::decode(bytes.first(AnimationCheckpoint::kBytes));
    if (!clock) {
        return std::nullopt;
    }
    ByteReader reader(bytes.subspan(AnimationCheckpoint::kBytes));
    State state;
    state.animation = *clock;
    bool badEnum = false;
    const auto enumeration = [&]<class Enum>(Enum& value) {
        const u32 raw = reader.readU32();
        // Reject before narrowing; a value of 256 must never become enum zero.
        badEnum = badEnum || raw > 255;
        value = static_cast<Enum>(raw & 255U);
    };
    enumeration(state.action);
    enumeration(state.footfall);
    enumeration(state.meleeBlow);
    enumeration(state.melee.range);
    const u32 low = reader.readU32();
    if (low > 1) {
        return std::nullopt;
    }
    state.melee.low = low != 0;
    state.melee.yaw = real(reader);
    state.character = std::bit_cast<s32>(reader.readU32());
    state.chain = std::bit_cast<s32>(reader.readU32());
    state.potionThrowTicks = std::bit_cast<s32>(reader.readU32());
    state.comboClass = std::bit_cast<s32>(reader.readU32());
    state.stillTicks = std::bit_cast<s32>(reader.readU32());
    state.fidgetTicks = std::bit_cast<s32>(reader.readU32());
    enumeration(state.itemReleased);
    enumeration(state.strafe);
    state.guardArmor = real(reader);
    state.attackSeconds = real(reader);
    state.phaseSeconds = std::bit_cast<f64>(wide(reader));
    state.shotFrame = real(reader);
    const u32 flags = reader.readU32();
    if ((flags >> kFlags.size()) != 0) {
        return std::nullopt;
    }
    for (usize i = 0; i < kFlags.size(); ++i) {
        state.*kFlags[i] = (flags & (1U << i)) != 0;
    }
    return !badEnum && state.valid() ? std::optional{state} : std::nullopt;
}
} // namespace gdl::game
