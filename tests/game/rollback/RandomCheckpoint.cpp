#include "game/rollback/RandomCheckpoint.h"

#include "engine/io/ByteReader.h"

namespace gdl::game {

std::optional<std::vector<u8>> RandomCheckpoint::encode(const Mt19937::Checkpoint& state) {
    if (!state.valid()) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(kBytes);
    const auto word = [&](u32 value) {
        for (u32 shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<u8>(value >> shift));
        }
    };
    for (const u32 value : state.words) {
        word(value);
    }
    word(state.cursor);
    return bytes;
}

std::optional<Mt19937::Checkpoint> RandomCheckpoint::decode(std::span<const u8> bytes) {
    if (bytes.size() != kBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    Mt19937::Checkpoint state;
    for (u32& word : state.words) {
        word = reader.readU32();
    }
    state.cursor = reader.readU32();
    return state.valid() ? std::optional{state} : std::nullopt;
}

} // namespace gdl::game
