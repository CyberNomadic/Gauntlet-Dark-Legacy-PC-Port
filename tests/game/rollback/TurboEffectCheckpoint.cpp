#include "game/rollback/TurboEffectCheckpoint.h"

#include <bit>
#include <cmath>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
void bank(std::vector<u8>& bytes, const std::optional<TextureAnimator::Checkpoint>& value) {
    if (value) {
        word(bytes, value->frame);
        word(bytes, std::bit_cast<u32>(value->remainder));
        word(bytes, std::bit_cast<u32>(value->advance));
    }
}
TextureAnimator::Checkpoint bank(ByteReader& reader) {
    return {reader.readU32(), std::bit_cast<f32>(reader.readU32()),
            std::bit_cast<f32>(reader.readU32())};
}
} // namespace

bool TurboEffectState::valid() const {
    return std::isfinite(meleeDamage) && meleeDamage >= 0 && meleeDamage <= kMaxDamage &&
           (!classBank || classBank->valid()) && (!weaponBank || weaponBank->valid());
}

std::optional<std::vector<u8>> TurboEffectCheckpoint::encode(const TurboEffectState& state) {
    if (!state.valid()) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(kMaxBytes);
    word(bytes, (state.classBank ? 1U : 0U) | (state.weaponBank ? 2U : 0U));
    word(bytes, std::bit_cast<u32>(state.meleeDamage));
    bank(bytes, state.classBank);
    bank(bytes, state.weaponBank);
    return bytes;
}

std::optional<TurboEffectState> TurboEffectCheckpoint::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    const u32 flags = reader.readU32();
    if ((flags & ~3U) != 0 ||
        bytes.size() != kHeaderBytes + static_cast<usize>(std::popcount(flags)) * kBankBytes) {
        return std::nullopt;
    }
    TurboEffectState state;
    state.meleeDamage = std::bit_cast<f32>(reader.readU32());
    if ((flags & 1U) != 0) {
        state.classBank = bank(reader);
    }
    if ((flags & 2U) != 0) {
        state.weaponBank = bank(reader);
    }
    return state.valid() ? std::optional{state} : std::nullopt;
}
} // namespace gdl::game
