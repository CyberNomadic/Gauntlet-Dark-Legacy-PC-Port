#include "game/netplay/PlayerMeleeCheckpoint.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
bool bounded(f32 value, f32 low, f32 high) {
    return std::isfinite(value) && value >= low && value <= high;
}
bool vectorValid(const Vec3& value) {
    return bounded(value.x, -1'000'000, 1'000'000) && bounded(value.y, -1'000'000, 1'000'000) &&
           bounded(value.z, -1'000'000, 1'000'000);
}
bool targetValid(const MeleeSelection::Target& value) {
    return vectorValid(value.base) && vectorValid(value.offset) &&
           bounded(value.distance, -1024, 1'000'000) && bounded(value.score, -1024, 1'000'000);
}
bool geometryValid(const MeleeGeometry::Target& value) {
    const auto& acquisition = value.acquisition;
    return value.id >= 0 && value.id <= 65535 && vectorValid(value.base) &&
           bounded(value.radius, 0.01f, 100) && bounded(value.height, 0.01f, 100) &&
           vectorValid(acquisition.point) && bounded(acquisition.radius, 0, 100) &&
           bounded(acquisition.distanceScale, 0.01f, 16) &&
           bounded(acquisition.maxHeight, 0, 1000) &&
           glm::distance(acquisition.point, value.base) <= 100 && value.contactCell >= 0 &&
           value.contactCell < 4096 && (!value.contact || value.instance != 0) &&
           (value.contact || (!value.requireFacing && value.contactCell == 0));
}
bool obstacleValid(const Obstacle& value) {
    return vectorValid(value.centre) && bounded(value.yaw, -kTwoPi, kTwoPi) &&
           bounded(value.halfAcross, 0, 10000) && bounded(value.halfAlong, 0, 10000) &&
           bounded(value.height, 0, 10000) && bounded(value.cylinderRadius, 0, 10000) &&
           (!value.enemyItem || bounded(value.enemyItem->radius, 0, 10000));
}
bool movingValid(const PlayerMeleeState::Moving& value, const Vec3& feet) {
    if (value.origin != feet || !bounded(value.radius, 0.01f, PlayerMeleeState::kMaxRadius) ||
        value.ticks == 0 || value.ticks > PlayerMeleeState::kMaxTicks ||
        value.targets.size() > PlayerMeleeState::kMaxTargets ||
        value.obstacles.size() > PlayerMeleeState::kMaxObstacles ||
        !std::ranges::all_of(value.targets, geometryValid) ||
        !std::ranges::all_of(value.obstacles, obstacleValid)) {
        return false;
    }
    constexpr f32 kMaximumQueryDistance = 256;
    for (usize index = 0; index < value.targets.size(); ++index) {
        const auto& target = value.targets[index];
        if (glm::distance(target.acquisition.point, feet) > kMaximumQueryDistance) {
            return false;
        }
        for (usize previous = 0; previous < index; ++previous) {
            const auto& prior = value.targets[previous];
            if (prior.id == target.id ||
                (prior.contact && target.contact && prior.instance == target.instance)) {
                return false;
            }
        }
    }
    return true;
}
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
void target(std::vector<u8>& bytes, const MeleeSelection::Target& value) {
    vector(bytes, value.base);
    vector(bytes, value.offset);
    real(bytes, value.distance);
    real(bytes, value.score);
    word(bytes, value.low ? 1U : 0U);
}
std::optional<MeleeSelection::Target> target(ByteReader& reader) {
    MeleeSelection::Target result;
    result.base = vector(reader);
    result.offset = vector(reader);
    result.distance = real(reader);
    result.score = real(reader);
    const u32 low = reader.readU32();
    if (low > 1) {
        return std::nullopt;
    }
    result.low = low != 0;
    return result;
}
void geometry(std::vector<u8>& bytes, const MeleeGeometry::Target& value) {
    word(bytes, static_cast<u32>(value.id));
    word(bytes, static_cast<u32>(value.instance));
    word(bytes, static_cast<u32>(value.instance >> 32));
    vector(bytes, value.base);
    real(bytes, value.radius);
    real(bytes, value.height);
    vector(bytes, value.acquisition.point);
    real(bytes, value.acquisition.radius);
    real(bytes, value.acquisition.distanceScale);
    real(bytes, value.acquisition.maxHeight);
    word(bytes, (value.acquisition.enabled ? 1U : 0U) | (value.contact ? 2U : 0U) |
                    (value.requireFacing ? 4U : 0U) | (value.low ? 8U : 0U) |
                    (value.blocksMovement ? 16U : 0U));
    word(bytes, static_cast<u32>(value.contactCell));
}
std::optional<MeleeGeometry::Target> geometry(ByteReader& reader) {
    MeleeGeometry::Target result;
    result.id = std::bit_cast<s32>(reader.readU32());
    const u64 low = reader.readU32();
    result.instance = low | (static_cast<u64>(reader.readU32()) << 32);
    result.base = vector(reader);
    result.radius = real(reader);
    result.height = real(reader);
    result.acquisition.point = vector(reader);
    result.acquisition.radius = real(reader);
    result.acquisition.distanceScale = real(reader);
    result.acquisition.maxHeight = real(reader);
    const u32 flags = reader.readU32();
    if (flags > 31) {
        return std::nullopt;
    }
    result.acquisition.enabled = (flags & 1U) != 0;
    result.contact = (flags & 2U) != 0;
    result.requireFacing = (flags & 4U) != 0;
    result.low = (flags & 8U) != 0;
    result.blocksMovement = (flags & 16U) != 0;
    result.contactCell = std::bit_cast<s32>(reader.readU32());
    return result;
}
void obstacle(std::vector<u8>& bytes, const Obstacle& value) {
    vector(bytes, value.centre);
    real(bytes, value.yaw);
    real(bytes, value.halfAcross);
    real(bytes, value.halfAlong);
    real(bytes, value.height);
    real(bytes, value.cylinderRadius);
    word(bytes, (value.solid ? 1U : 0U) | (value.enemyItem ? 2U : 0U) |
                    (value.enemyItem && value.enemyItem->cylindrical ? 4U : 0U));
    real(bytes, value.enemyItem ? value.enemyItem->radius : 0);
}
std::optional<Obstacle> obstacle(ByteReader& reader) {
    Obstacle result;
    result.centre = vector(reader);
    result.yaw = real(reader);
    result.halfAcross = real(reader);
    result.halfAlong = real(reader);
    result.height = real(reader);
    result.cylinderRadius = real(reader);
    const u32 flags = reader.readU32();
    const f32 radius = real(reader);
    if (flags > 7 || ((flags & 2U) == 0 && ((flags & 4U) != 0 || radius != 0))) {
        return std::nullopt;
    }
    result.solid = (flags & 1U) != 0;
    if ((flags & 2U) != 0) {
        result.enemyItem = Obstacle::ItemQuery{(flags & 4U) != 0, radius};
    }
    return result;
}
} // namespace

bool PlayerMeleeState::valid() const {
    return vectorValid(selection.feet) && bounded(selection.reach, 0.02f, 100) &&
           bounded(selection.range, 0.01f, 200) && bounded(selection.facingDot, -1, 1) &&
           bounded(captureYaw, -kTwoPi, kTwoPi) && (!assistedAim || vectorValid(*assistedAim)) &&
           selection.targets.size() <= kMaxTargets &&
           std::ranges::all_of(selection.targets, targetValid) &&
           (!selection.contact || targetValid(selection.contact->target)) &&
           (!moving || movingValid(*moving, selection.feet));
}

std::optional<std::vector<u8>> PlayerMeleeCheckpoint::encode(const PlayerMeleeState& state) {
    if (!state.valid()) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(kHeaderBytes + state.selection.targets.size() * kTargetBytes);
    vector(bytes, state.selection.feet);
    real(bytes, state.selection.reach);
    real(bytes, state.selection.range);
    real(bytes, state.selection.facingDot);
    real(bytes, state.captureYaw);
    word(bytes, (state.assistedAim ? 1U : 0U) | (state.selection.contact ? 2U : 0U) |
                    (state.moving ? 4U : 0U));
    word(bytes, static_cast<u32>(state.selection.targets.size()));
    vector(bytes, state.assistedAim.value_or(Vec3{0}));
    if (state.selection.contact) {
        target(bytes, state.selection.contact->target);
        word(bytes, state.selection.contact->requireFacing ? 1U : 0U);
    } else {
        bytes.insert(bytes.end(), kTargetBytes + 4, 0);
    }
    for (const auto& value : state.selection.targets) {
        target(bytes, value);
    }
    if (state.moving) {
        const auto& moving = *state.moving;
        vector(bytes, moving.origin);
        real(bytes, moving.radius);
        word(bytes, moving.ticks);
        word(bytes, static_cast<u32>(moving.targets.size()));
        word(bytes, static_cast<u32>(moving.obstacles.size()));
        for (const auto& value : moving.targets) {
            geometry(bytes, value);
        }
        for (const auto& value : moving.obstacles) {
            obstacle(bytes, value);
        }
    }
    return bytes;
}

std::optional<PlayerMeleeState> PlayerMeleeCheckpoint::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    PlayerMeleeState state;
    state.selection.feet = vector(reader);
    state.selection.reach = real(reader);
    state.selection.range = real(reader);
    state.selection.facingDot = real(reader);
    state.captureYaw = real(reader);
    const u32 flags = reader.readU32();
    const usize count = reader.readU32();
    if (flags > 7 || count > PlayerMeleeState::kMaxTargets) {
        return std::nullopt;
    }
    const usize stationaryBytes = kHeaderBytes + count * kTargetBytes;
    if (((flags & 4U) == 0 && bytes.size() != stationaryBytes) ||
        ((flags & 4U) != 0 && bytes.size() < stationaryBytes + kMovingHeaderBytes)) {
        return std::nullopt;
    }
    const auto aim = vector(reader);
    if ((flags & 1U) != 0) {
        state.assistedAim = aim;
    } else if (aim != Vec3{0}) {
        return std::nullopt;
    }
    if ((flags & 2U) != 0) {
        const auto value = target(reader);
        const u32 facing = reader.readU32();
        if (!value || facing > 1) {
            return std::nullopt;
        }
        state.selection.contact = MeleeSelection::Contact{*value, facing != 0};
    } else {
        const auto absent = reader.readBytes(kTargetBytes + 4);
        if (std::ranges::any_of(absent, [](u8 value) { return value != 0; })) {
            return std::nullopt;
        }
    }
    state.selection.targets.reserve(count);
    for (usize i = 0; i < count; ++i) {
        const auto value = target(reader);
        if (!value) {
            return std::nullopt;
        }
        state.selection.targets.push_back(*value);
    }
    if ((flags & 4U) != 0) {
        PlayerMeleeState::Moving moving;
        moving.origin = vector(reader);
        moving.radius = real(reader);
        moving.ticks = reader.readU32();
        const usize targets = reader.readU32();
        const usize obstacles = reader.readU32();
        if (targets > PlayerMeleeState::kMaxTargets ||
            obstacles > PlayerMeleeState::kMaxObstacles ||
            bytes.size() != stationaryBytes + kMovingHeaderBytes + targets * kGeometryBytes +
                                obstacles * kObstacleBytes) {
            return std::nullopt;
        }
        moving.targets.reserve(targets);
        moving.obstacles.reserve(obstacles);
        for (usize i = 0; i < targets; ++i) {
            const auto value = geometry(reader);
            if (!value) {
                return std::nullopt;
            }
            moving.targets.push_back(*value);
        }
        for (usize i = 0; i < obstacles; ++i) {
            const auto value = obstacle(reader);
            if (!value) {
                return std::nullopt;
            }
            moving.obstacles.push_back(*value);
        }
        state.moving = std::move(moving);
    }
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}
} // namespace gdl::game
