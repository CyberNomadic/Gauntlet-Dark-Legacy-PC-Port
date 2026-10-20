#include "game/netplay/PlacedEnemyCheckpoint.h"

#include <bit>
#include <utility>

#include "engine/io/ByteReader.h"

#include "game/netplay/CanonicalDigestSink.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDPL");

template <class Output> void word(Output& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
template <class Output> void integer(Output& bytes, s32 value) {
    word(bytes, std::bit_cast<u32>(value));
}
s32 integer(ByteReader& reader) {
    return std::bit_cast<s32>(reader.readU32());
}
template <class Output> void real(Output& bytes, f32 value) {
    word(bytes, std::bit_cast<u32>(value));
}
f32 real(ByteReader& reader) {
    return std::bit_cast<f32>(reader.readU32());
}
template <class Output> void vector(Output& bytes, const Vec3& value) {
    real(bytes, value.x);
    real(bytes, value.y);
    real(bytes, value.z);
}
Vec3 vector(ByteReader& reader) {
    return {real(reader), real(reader), real(reader)};
}
template <class Output> void view(Output& bytes, const ViewVolume& value) {
    vector(bytes, value.position);
    vector(bytes, value.right);
    vector(bytes, value.up);
    vector(bytes, value.forward);
    real(bytes, value.sinX);
    real(bytes, value.cosX);
    real(bytes, value.sinY);
    real(bytes, value.cosY);
}
ViewVolume view(ByteReader& reader) {
    ViewVolume value;
    value.position = vector(reader);
    value.right = vector(reader);
    value.up = vector(reader);
    value.forward = vector(reader);
    value.sinX = real(reader);
    value.cosX = real(reader);
    value.sinY = real(reader);
    value.cosY = real(reader);
    return value;
}
template <class Output> void placement(Output& bytes, const PlacedEnemies::Placement& value) {
    const auto& spawn = value.spawn;
    integer(bytes, spawn.zigZagSide);
    integer(bytes, spawn.kind);
    integer(bytes, spawn.tier);
    integer(bytes, spawn.algorithm);
    vector(bytes, spawn.position);
    vector(bytes, spawn.direction);
    real(bytes, spawn.clearance);
    integer(bytes, spawn.generator);
    word(bytes, (spawn.placed ? 1U : 0U) | (spawn.patrolBirth ? 2U : 0U) |
                    (spawn.allBirthDirections ? 4U : 0U) | (spawn.asleep ? 8U : 0U) |
                    (value.carried ? 16U : 0U));
    real(bytes, spawn.throwInterval);
    word(bytes, static_cast<u32>(spawn.priority));
    real(bytes, spawn.sight);
    integer(bytes, value.kind);
    real(bytes, value.facing);
    real(bytes, value.viewRadius);
    real(bytes, value.sight);
    word(bytes, value.carried ? static_cast<u32>(*value.carried) : 0);
}
std::optional<PlacedEnemies::Placement> placement(ByteReader& reader) {
    PlacedEnemies::Placement value;
    auto& spawn = value.spawn;
    spawn.zigZagSide = integer(reader);
    spawn.kind = integer(reader);
    spawn.tier = integer(reader);
    spawn.algorithm = integer(reader);
    spawn.position = vector(reader);
    spawn.direction = vector(reader);
    spawn.clearance = real(reader);
    spawn.generator = integer(reader);
    const u32 flags = reader.readU32();
    if ((flags & ~31U) != 0) {
        return std::nullopt;
    }
    spawn.placed = (flags & 1U) != 0;
    spawn.patrolBirth = (flags & 2U) != 0;
    spawn.allBirthDirections = (flags & 4U) != 0;
    spawn.asleep = (flags & 8U) != 0;
    spawn.throwInterval = real(reader);
    const u32 priority = reader.readU32();
    if (priority > static_cast<u32>(EnemySpawn::Priority::Visible)) {
        return std::nullopt;
    }
    spawn.priority = static_cast<EnemySpawn::Priority>(priority);
    spawn.sight = real(reader);
    value.kind = integer(reader);
    value.facing = real(reader);
    value.viewRadius = real(reader);
    value.sight = real(reader);
    const u32 carried = reader.readU32();
    if ((flags & 16U) != 0) {
        value.carried = carried;
    } else if (carried != 0) {
        return std::nullopt;
    }
    return PlacedEnemiesCheckpoint::validPlacement(value) ? std::optional{value} : std::nullopt;
}
} // namespace

namespace {
template <class Output>
bool writeCanonical(Output& bytes, const PlacedEnemiesCheckpoint::State& state, bool validate) {
    if ((validate && !state.valid()) ||
        state.roster.size() > PlacedEnemyCheckpoint::kMaxPlacements ||
        state.pending.size() > PlacedEnemyCheckpoint::kMaxPlacements) {
        return false;
    }
    bytes.reserve(
        PlacedEnemyCheckpoint::kHeaderBytes + (state.view ? PlacedEnemyCheckpoint::kViewBytes : 0) +
        state.roster.size() * PlacedEnemyCheckpoint::kPlacementBytes + state.pending.size() * 4);
    word(bytes, kMagic);
    word(bytes, PlacedEnemyCheckpoint::kVersion);
    word(bytes, static_cast<u32>(state.roster.size()));
    word(bytes, static_cast<u32>(state.pending.size()));
    word(bytes, state.view ? 1U : 0U);
    vector(bytes, state.attention);
    if (state.view) {
        view(bytes, *state.view);
    }
    for (const auto& value : state.roster) {
        placement(bytes, value);
    }
    for (const auto index : state.pending) {
        word(bytes, static_cast<u32>(index));
    }
    return true;
}
} // namespace

std::optional<std::vector<u8>>
PlacedEnemyCheckpoint::encode(const PlacedEnemiesCheckpoint::State& state) {
    std::vector<u8> bytes;
    return writeCanonical(bytes, state, true) ? std::optional{std::move(bytes)} : std::nullopt;
}

std::optional<std::array<u8, 32>>
PlacedEnemyCheckpoint::canonicalDigest(const PlacedEnemiesCheckpoint::State& state) {
    CanonicalDigestSink sink;
    return writeCanonical(sink, state, false) ? std::optional{sink.finish()} : std::nullopt;
}

std::optional<PlacedEnemiesCheckpoint::State> PlacedEnemyCheckpoint::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    if (reader.readU32() != kMagic || reader.readU32() != kVersion) {
        return std::nullopt;
    }
    const usize count = reader.readU32();
    const usize pending = reader.readU32();
    const u32 flags = reader.readU32();
    if (count > kMaxPlacements || pending > count || flags > 1 ||
        bytes.size() !=
            kHeaderBytes + (flags != 0 ? kViewBytes : 0) + count * kPlacementBytes + pending * 4) {
        return std::nullopt;
    }
    PlacedEnemiesCheckpoint::State state;
    state.attention = vector(reader);
    if (flags != 0) {
        state.view = view(reader);
    }
    state.roster.reserve(count);
    for (usize index = 0; index < count; ++index) {
        auto value = placement(reader);
        if (!value) {
            return std::nullopt;
        }
        state.roster.push_back(*value);
    }
    state.pending.reserve(pending);
    for (usize index = 0; index < pending; ++index) {
        state.pending.push_back(reader.readU32());
    }
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}

} // namespace gdl::game
