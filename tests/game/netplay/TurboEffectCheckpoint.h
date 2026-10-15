#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"
#include "engine/world/TextureAnimator.h"

namespace gdl::game {
/** The exact shared native texture banks at a completed host tick. An absent
 * bank explicitly has never started; an absent enclosing descriptor is unknown.
 * This is not a checkpoint of already-live effects or standalone particle RNG. */
struct TurboEffectState {
    static constexpr f32 kMaxDamage = 1'000'000;
    f32 meleeDamage = 0;
    std::optional<TextureAnimator::Checkpoint> classBank;
    std::optional<TextureAnimator::Checkpoint> weaponBank;
    bool valid() const;
    bool operator==(const TurboEffectState&) const = default;
};

class TurboEffectCheckpoint {
public:
    static constexpr usize kHeaderBytes = 8;
    static constexpr usize kBankBytes = 12;
    static constexpr usize kMaxBytes = kHeaderBytes + 2 * kBankBytes;
    static std::optional<std::vector<u8>> encode(const TurboEffectState& state);
    static std::optional<TurboEffectState> decode(std::span<const u8> bytes);
};
} // namespace gdl::game
