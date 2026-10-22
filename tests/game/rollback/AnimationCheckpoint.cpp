#include "game/rollback/AnimationCheckpoint.h"

#include <bit>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
void wide(std::vector<u8>& bytes, u64 value) {
    word(bytes, static_cast<u32>(value));
    word(bytes, static_cast<u32>(value >> 32));
}
u64 wide(ByteReader& reader) {
    const u64 low = reader.readU32();
    return low | (static_cast<u64>(reader.readU32()) << 32);
}
} // namespace

std::optional<std::vector<u8>>
AnimationCheckpoint::encode(const AnimationPlayer::Checkpoint& state) {
    if (!state.valid()) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(kBytes);
    word(bytes, state.sequence);
    wide(bytes, state.generation);
    word(bytes, std::bit_cast<u32>(state.secondsPerFrame));
    wide(bytes, std::bit_cast<u64>(state.time));
    for (const f32 value :
         {state.frame, state.speed, state.transitionLength, state.transitionTime}) {
        word(bytes, std::bit_cast<u32>(value));
    }
    word(bytes, (state.playing ? 1U : 0U) | (state.finished ? 2U : 0U) | (state.held ? 4U : 0U) |
                    (state.smooth ? 8U : 0U));
    return bytes;
}

std::optional<AnimationPlayer::Checkpoint> AnimationCheckpoint::decode(std::span<const u8> bytes) {
    if (bytes.size() != kBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    AnimationPlayer::Checkpoint state;
    state.sequence = reader.readU32();
    state.generation = wide(reader);
    state.secondsPerFrame = std::bit_cast<f32>(reader.readU32());
    state.time = std::bit_cast<f64>(wide(reader));
    state.frame = std::bit_cast<f32>(reader.readU32());
    state.speed = std::bit_cast<f32>(reader.readU32());
    state.transitionLength = std::bit_cast<f32>(reader.readU32());
    state.transitionTime = std::bit_cast<f32>(reader.readU32());
    const u32 flags = reader.readU32();
    if ((flags & ~15U) != 0) {
        return std::nullopt;
    }
    state.playing = (flags & 1U) != 0;
    state.finished = (flags & 2U) != 0;
    state.held = (flags & 4U) != 0;
    state.smooth = (flags & 8U) != 0;
    return state.valid() ? std::optional{state} : std::nullopt;
}
} // namespace gdl::game
