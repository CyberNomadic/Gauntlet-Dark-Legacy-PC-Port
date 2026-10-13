#include "game/netplay/PlayerLocomotionCheckpoint.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

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
bool bodyValid(const PlayerLocomotionState::Body& value) {
    return vectorValid(value.base) && bounded(value.radius, 0.01f, 100) &&
           bounded(value.height, 0.01f, 1000) && value.identity.valid() &&
           value.identity.kind != PlayerLocomotionState::Identity::Kind::Generator;
}
template <class Range, class IdentityOf>
bool uniqueIdentities(const Range& values, IdentityOf identityOf) {
    for (usize index = 0; index < values.size(); ++index) {
        const auto& identity = identityOf(values[index]);
        if (identity.kind == PlayerLocomotionState::Identity::Kind::Anonymous) {
            continue;
        }
        for (usize previous = 0; previous < index; ++previous) {
            const auto& other = identityOf(values[previous]);
            // One live native slot cannot contain two lifetimes simultaneously.
            if (identity.kind == other.kind && identity.slot == other.slot) {
                return false;
            }
        }
    }
    return true;
}
bool obstacleValid(const Obstacle& value) {
    return vectorValid(value.centre) && bounded(value.yaw, -kTwoPi, kTwoPi) &&
           bounded(value.halfAcross, 0, 10000) && bounded(value.halfAlong, 0, 10000) &&
           bounded(value.height, 0, 10000) && bounded(value.cylinderRadius, 0, 10000) &&
           (!value.enemyItem || bounded(value.enemyItem->radius, 0, 10000));
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
void identity(std::vector<u8>& bytes, const PlayerLocomotionState::Identity& value) {
    word(bytes, static_cast<u32>(value.kind));
    word(bytes, value.slot);
    word(bytes, static_cast<u32>(value.instance));
    word(bytes, static_cast<u32>(value.instance >> 32));
}
PlayerLocomotionState::Identity identity(ByteReader& reader) {
    PlayerLocomotionState::Identity value;
    value.kind = static_cast<PlayerLocomotionState::Identity::Kind>(reader.readU32());
    value.slot = reader.readU32();
    value.instance = reader.readU32();
    value.instance |= static_cast<u64>(reader.readU32()) << 32;
    return value;
}
void body(std::vector<u8>& bytes, const PlayerLocomotionState::Body& value) {
    vector(bytes, value.base);
    real(bytes, value.radius);
    real(bytes, value.height);
    identity(bytes, value.identity);
}
PlayerLocomotionState::Body body(ByteReader& reader) {
    return {vector(reader), real(reader), real(reader), identity(reader)};
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

bool PlayerLocomotionState::Identity::valid() const {
    switch (kind) {
    case Kind::Anonymous: return slot == 0 && instance == 0;
    case Kind::Swarm:
        return slot <= static_cast<u32>(std::numeric_limits<s32>::max()) && instance != 0;
    case Kind::Generator:
        return slot <= static_cast<u32>(std::numeric_limits<s32>::max()) && instance == 0;
    }
    return false;
}

bool PlayerLocomotionState::valid() const {
    if (!vectorValid(origin) || !bounded(radius, 0.01f, kMaxRadius) || ticks == 0 ||
        ticks > kMaxTicks || bodies.size() > kMaxBodies || generators.size() > kMaxGenerators ||
        fixtures.size() > kMaxFixtures || party.size() > kMaxParty ||
        !std::ranges::all_of(bodies, bodyValid) || !std::ranges::all_of(party, bodyValid) ||
        !std::ranges::all_of(generators, obstacleValid) ||
        !std::ranges::all_of(fixtures, obstacleValid)) {
        return false;
    }
    if ((!generatorIdentities.empty() && generatorIdentities.size() != generators.size()) ||
        !std::ranges::all_of(generatorIdentities,
                             [](const auto& value) {
                                 return value.valid() && value.kind != Identity::Kind::Swarm;
                             }) ||
        !std::ranges::all_of(
            party,
            [](const auto& value) { return value.identity.kind == Identity::Kind::Anonymous; }) ||
        !uniqueIdentities(bodies,
                          [](const auto& value) -> const auto& { return value.identity; }) ||
        !uniqueIdentities(generatorIdentities,
                          [](const auto& value) -> const auto& { return value; })) {
        return false;
    }
    return !automaticGeometry ||
           (automatic && automaticGeometry->valid() &&
            automaticGeometry->selection.feet == origin && automaticGeometry->moving &&
            automaticGeometry->moving->radius >= radius &&
            automaticGeometry->moving->ticks >= ticks);
}

std::optional<std::vector<u8>>
PlayerLocomotionCheckpoint::encode(const PlayerLocomotionState& state) {
    if (!state.valid()) {
        return std::nullopt;
    }
    std::vector<u8> acquisition;
    if (state.automaticGeometry) {
        const auto encoded = PlayerMeleeCheckpoint::encode(*state.automaticGeometry);
        if (!encoded) {
            return std::nullopt;
        }
        acquisition = *encoded;
    }
    std::vector<u8> bytes;
    bytes.reserve(kHeaderBytes + (state.bodies.size() + state.party.size()) * kBodyBytes +
                  (state.generators.size() + state.fixtures.size()) * kObstacleBytes +
                  state.generators.size() * kIdentityBytes + acquisition.size());
    vector(bytes, state.origin);
    real(bytes, state.radius);
    word(bytes, state.ticks);
    word(bytes, static_cast<u32>(state.bodies.size()));
    word(bytes, static_cast<u32>(state.generators.size()));
    word(bytes, static_cast<u32>(state.fixtures.size()));
    word(bytes, static_cast<u32>(state.party.size()));
    word(bytes, state.automatic ? 1U : 0U);
    word(bytes, static_cast<u32>(acquisition.size()));
    for (const auto& value : state.bodies) {
        body(bytes, value);
    }
    for (usize index = 0; index < state.generators.size(); ++index) {
        obstacle(bytes, state.generators[index]);
        identity(bytes, state.generatorIdentities.empty() ? PlayerLocomotionState::Identity{}
                                                          : state.generatorIdentities[index]);
    }
    for (const auto& value : state.fixtures) {
        obstacle(bytes, value);
    }
    for (const auto& value : state.party) {
        body(bytes, value);
    }
    bytes.insert(bytes.end(), acquisition.begin(), acquisition.end());
    return bytes;
}

std::optional<PlayerLocomotionState> PlayerLocomotionCheckpoint::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    PlayerLocomotionState state;
    state.origin = vector(reader);
    state.radius = real(reader);
    state.ticks = reader.readU32();
    const usize bodies = reader.readU32();
    const usize generators = reader.readU32();
    const usize fixtures = reader.readU32();
    const usize party = reader.readU32();
    const u32 flags = reader.readU32();
    const usize acquisition = reader.readU32();
    if (bodies > PlayerLocomotionState::kMaxBodies ||
        generators > PlayerLocomotionState::kMaxGenerators ||
        fixtures > PlayerLocomotionState::kMaxFixtures ||
        party > PlayerLocomotionState::kMaxParty || flags > 1 ||
        acquisition > PlayerMeleeCheckpoint::kMaxBytes ||
        (acquisition != 0 && (flags == 0 || acquisition < PlayerMeleeCheckpoint::kHeaderBytes)) ||
        bytes.size() != kHeaderBytes + (bodies + party) * kBodyBytes +
                            (generators + fixtures) * kObstacleBytes + generators * kIdentityBytes +
                            acquisition) {
        return std::nullopt;
    }
    state.automatic = flags != 0;
    state.bodies.reserve(bodies);
    for (usize index = 0; index < bodies; ++index) {
        state.bodies.push_back(body(reader));
    }
    const auto boxes = [&](std::vector<Obstacle>& output, usize count) {
        output.reserve(count);
        for (usize index = 0; index < count; ++index) {
            const auto value = obstacle(reader);
            if (!value) {
                return false;
            }
            output.push_back(*value);
        }
        return true;
    };
    state.generators.reserve(generators);
    state.generatorIdentities.reserve(generators);
    for (usize index = 0; index < generators; ++index) {
        const auto value = obstacle(reader);
        if (!value) {
            return std::nullopt;
        }
        state.generators.push_back(*value);
        state.generatorIdentities.push_back(identity(reader));
    }
    if (std::ranges::all_of(state.generatorIdentities, [](const auto& value) {
            return value == PlayerLocomotionState::Identity{};
        })) {
        state.generatorIdentities.clear();
    }
    if (!boxes(state.fixtures, fixtures)) {
        return std::nullopt;
    }
    state.party.reserve(party);
    for (usize index = 0; index < party; ++index) {
        state.party.push_back(body(reader));
    }
    if (acquisition != 0) {
        state.automaticGeometry = PlayerMeleeCheckpoint::decode(reader.readBytes(acquisition));
        if (!state.automaticGeometry) {
            return std::nullopt;
        }
    }
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}
} // namespace gdl::game
