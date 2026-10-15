#include "game/netplay/PlayerKnockbackCheckpoint.h"

#include <bit>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
void real(std::vector<u8>& bytes, f32 value) {
    word(bytes, std::bit_cast<u32>(value));
}
f32 real(ByteReader& reader) {
    return std::bit_cast<f32>(reader.readU32());
}
void vector(std::vector<u8>& bytes, const Vec3& value) {
    real(bytes, value.x);
    real(bytes, value.y);
    real(bytes, value.z);
}
Vec3 vector(ByteReader& reader) {
    return {real(reader), real(reader), real(reader)};
}
} // namespace

std::optional<std::vector<u8>>
PlayerKnockbackCheckpoint::encode(const Knockback::Checkpoint& state) {
    if (!Knockback::valid(state)) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(kBytes);
    vector(bytes, state.force);
    word(bytes, state.flags);
    real(bytes, state.damage);
    vector(bytes, state.velocity);
    real(bytes, state.fastSeconds);
    real(bytes, state.frameSeconds);
    vector(bytes, state.shove);
    word(bytes, (state.shoved ? 1U : 0U) | (state.pushed ? 2U : 0U));
    return bytes;
}

std::optional<Knockback::Checkpoint> PlayerKnockbackCheckpoint::decode(std::span<const u8> bytes) {
    if (bytes.size() != kBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    Knockback::Checkpoint state;
    state.force = vector(reader);
    state.flags = reader.readU32();
    state.damage = real(reader);
    state.velocity = vector(reader);
    state.fastSeconds = real(reader);
    state.frameSeconds = real(reader);
    state.shove = vector(reader);
    const u32 latches = reader.readU32();
    if ((latches & ~3U) != 0) {
        return std::nullopt;
    }
    state.shoved = (latches & 1U) != 0;
    state.pushed = (latches & 2U) != 0;
    return Knockback::valid(state) ? std::optional{state} : std::nullopt;
}
} // namespace gdl::game
