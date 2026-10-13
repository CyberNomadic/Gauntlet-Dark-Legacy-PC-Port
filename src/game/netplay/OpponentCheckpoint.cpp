#include "game/netplay/OpponentCheckpoint.h"

#include <utility>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDLO");
constexpr u32 kVersion = 1;

void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
} // namespace

std::optional<std::vector<u8>> OpponentCheckpoint::encode(const OpponentState& state) {
    if (!state.valid()) {
        return std::nullopt;
    }
    const auto generators = GeneratorCheckpoint::encode(state.generators);
    const auto enemies = EnemyPoolCheckpoint::encode(state.enemies);
    const auto missiles = EnemyMissilesCheckpoint::encode(state.missiles);
    if (!generators || !enemies || !missiles) {
        return std::nullopt;
    }
    const usize size = kHeaderBytes + generators->size() + enemies->size() + missiles->size();
    if (size > kMaxBytes) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(size);
    word(bytes, kMagic);
    word(bytes, kVersion);
    word(bytes, static_cast<u32>(generators->size()));
    word(bytes, static_cast<u32>(enemies->size()));
    word(bytes, static_cast<u32>(missiles->size()));
    bytes.insert(bytes.end(), generators->begin(), generators->end());
    bytes.insert(bytes.end(), enemies->begin(), enemies->end());
    bytes.insert(bytes.end(), missiles->begin(), missiles->end());
    return bytes;
}

std::optional<OpponentState> OpponentCheckpoint::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes.first(kHeaderBytes));
    if (reader.readU32() != kMagic || reader.readU32() != kVersion) {
        return std::nullopt;
    }
    const usize generatorsSize = reader.readU32();
    const usize enemiesSize = reader.readU32();
    const usize missilesSize = reader.readU32();
    if (generatorsSize == 0 || generatorsSize > GeneratorCheckpoint::kMaxBytes ||
        enemiesSize == 0 || enemiesSize > EnemyPoolCheckpoint::kMaxBytes || missilesSize == 0 ||
        missilesSize > EnemyMissilesCheckpoint::kMaxBytes ||
        static_cast<u64>(generatorsSize) + enemiesSize + missilesSize !=
            bytes.size() - kHeaderBytes) {
        return std::nullopt;
    }
    auto generators = GeneratorCheckpoint::decode(bytes.subspan(kHeaderBytes, generatorsSize));
    if (!generators) {
        return std::nullopt;
    }
    auto enemies =
        EnemyPoolCheckpoint::decode(bytes.subspan(kHeaderBytes + generatorsSize, enemiesSize));
    if (!enemies) {
        return std::nullopt;
    }
    auto missiles = EnemyMissilesCheckpoint::decode(
        bytes.subspan(kHeaderBytes + generatorsSize + enemiesSize, missilesSize));
    if (!missiles) {
        return std::nullopt;
    }
    OpponentState state{std::move(*generators), std::move(*enemies), std::move(*missiles)};
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}
} // namespace gdl::game
