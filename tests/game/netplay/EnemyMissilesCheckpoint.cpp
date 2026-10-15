#include "game/netplay/EnemyMissilesCheckpoint.h"

#include <bit>
#include <utility>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

#include "game/netplay/CanonicalDigestSink.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDEM");
constexpr u32 kVersion = 1;

template <class Output> void word(Output& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
template <class Output> void wide(Output& bytes, u64 value) {
    word(bytes, static_cast<u32>(value));
    word(bytes, static_cast<u32>(value >> 32));
}
u64 wide(ByteReader& reader) {
    const u64 low = reader.readU32();
    return low | (u64{reader.readU32()} << 32);
}
template <class Output> void integer(Output& bytes, s32 value) {
    word(bytes, std::bit_cast<u32>(value));
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
bool count(usize amount, usize& used, usize most) {
    if (amount > most - used) {
        return false;
    }
    used += amount;
    return true;
}

std::optional<usize> encodedSize(const EnemyMissiles::Checkpoint& state) {
    using Codec = EnemyMissilesCheckpoint;
    if (state.missiles.size() > Codec::kMaxMissiles || state.bursts.size() > Codec::kMaxBursts) {
        return std::nullopt;
    }
    usize events = 0;
    if (!count(state.hits.size(), events, Codec::kMaxEvents) ||
        !count(state.rockHits.size(), events, Codec::kMaxEvents) ||
        !count(state.pickupBlasts.size(), events, Codec::kMaxEvents)) {
        return std::nullopt;
    }
    usize size = Codec::kHeaderBytes + state.missiles.size() * Codec::kFlightBytes +
                 state.bursts.size() * Codec::kBurstBytes + state.hits.size() * Codec::kHitBytes +
                 state.rockHits.size() * Codec::kRockBytes +
                 state.pickupBlasts.size() * Codec::kPickupBytes;
    usize contacts = 0;
    usize stages = 0;
    for (const auto& flight : state.missiles) {
        if (flight.pierced.size() > 4 ||
            !count(flight.pierced.size(), contacts, Codec::kMaxContacts)) {
            return std::nullopt;
        }
        size += flight.pierced.size() * 8;
    }
    for (const auto& burst : state.bursts) {
        if (burst.players.size() > 4 ||
            !count(burst.players.size(), contacts, Codec::kMaxContacts) ||
            !count(burst.swarm.size(), contacts, Codec::kMaxContacts) ||
            !count(burst.scenery.size(), contacts, Codec::kMaxContacts) ||
            !count(burst.blast.stages.size(), stages, Codec::kMaxStages)) {
            return std::nullopt;
        }
        size += (burst.players.size() + burst.swarm.size()) * 8 + burst.scenery.size() * 4 +
                burst.blast.stages.size() * 4;
    }
    return size;
}
} // namespace

namespace {
template <class Output>
bool writeCanonical(Output& bytes, const EnemyMissiles::Checkpoint& state, bool validate) {
    const auto size = encodedSize(state);
    if (!size || (validate && !state.valid())) {
        return false;
    }
    const auto random = RandomCheckpoint::encode(state.random);
    if (!random) {
        return false;
    }
    bytes.reserve(*size);
    word(bytes, kMagic);
    word(bytes, kVersion);
    wide(bytes, state.nextInstance);
    real(bytes, state.shrink);
    real(bytes, state.ricochetIn);
    word(bytes, static_cast<u32>(state.missiles.size()));
    word(bytes, static_cast<u32>(state.bursts.size()));
    word(bytes, static_cast<u32>(state.hits.size()));
    word(bytes, static_cast<u32>(state.rockHits.size()));
    word(bytes, static_cast<u32>(state.pickupBlasts.size()));
    for (const u8 value : *random) {
        bytes.push_back(value);
    }
    for (const auto& flight : state.missiles) {
        wide(bytes, flight.instance);
        word(bytes, flight.continuity);
        vector(bytes, flight.position);
        vector(bytes, flight.velocity);
        vector(bytes, flight.turned);
        integer(bytes, flight.kind.slot);
        word(bytes, flight.kind.flags);
        real(bytes, flight.kind.damage);
        real(bytes, flight.kind.speed);
        real(bytes, flight.kind.radius);
        real(bytes, flight.kind.burstRadius);
        vector(bytes, flight.kind.spin);
        real(bytes, flight.kind.weight);
        real(bytes, flight.kind.held);
        word(bytes, flight.resource);
        integer(bytes, flight.shooter);
        real(bytes, flight.secondsLeft);
        real(bytes, flight.lived);
        real(bytes, flight.heldLeft);
        real(bytes, flight.scale);
        word(bytes, (flight.kind.throughWorld ? 1U : 0U) | (flight.reflected ? 2U : 0U));
        word(bytes, static_cast<u32>(flight.pierced.size()));
        for (const auto& [player, seconds] : flight.pierced) {
            integer(bytes, player);
            real(bytes, seconds);
        }
    }
    for (const auto& burst : state.bursts) {
        vector(bytes, burst.blast.position);
        real(bytes, burst.blast.radius);
        real(bytes, burst.blast.damage);
        word(bytes, burst.blast.flags);
        integer(bytes, burst.blast.spared);
        word(bytes, static_cast<u32>(burst.stage));
        real(bytes, burst.stageLeft);
        word(bytes, burst.blast.lit ? 1U : 0U);
        word(bytes, static_cast<u32>(burst.blast.stages.size()));
        word(bytes, static_cast<u32>(burst.players.size()));
        word(bytes, static_cast<u32>(burst.swarm.size()));
        word(bytes, static_cast<u32>(burst.scenery.size()));
        for (const f32 seconds : burst.blast.stages) {
            real(bytes, seconds);
        }
        for (const auto& held : burst.players) {
            integer(bytes, held.id);
            real(bytes, held.secondsLeft);
        }
        for (const auto& held : burst.swarm) {
            integer(bytes, held.id);
            real(bytes, held.secondsLeft);
        }
        for (const s32 id : burst.scenery) {
            integer(bytes, id);
        }
    }
    for (const auto& hit : state.hits) {
        integer(bytes, hit.player);
        integer(bytes, hit.shooter);
        real(bytes, hit.damage);
        word(bytes, hit.flags);
        real(bytes, hit.burstRadius);
        vector(bytes, hit.position);
        vector(bytes, hit.direction);
        word(bytes, (hit.worldContact ? 1U : 0U) | (hit.fromBurst ? 2U : 0U) |
                        (hit.ricochet ? 4U : 0U) | (hit.liquid ? 8U : 0U));
        integer(bytes, hit.target);
        integer(bytes, hit.worldObject);
    }
    for (const auto& hit : state.rockHits) {
        word(bytes, static_cast<u32>(hit.rock));
        real(bytes, hit.damage);
    }
    for (const auto& blast : state.pickupBlasts) {
        vector(bytes, blast.position);
        real(bytes, blast.radius);
        real(bytes, blast.damage);
        word(bytes, blast.flags);
    }
    return true;
}
} // namespace

std::optional<std::vector<u8>>
EnemyMissilesCheckpoint::encode(const EnemyMissiles::Checkpoint& state) {
    std::vector<u8> bytes;
    return writeCanonical(bytes, state, true) ? std::optional{std::move(bytes)} : std::nullopt;
}

std::optional<std::array<u8, 32>>
EnemyMissilesCheckpoint::canonicalDigest(const EnemyMissiles::Checkpoint& state) {
    CanonicalDigestSink sink;
    return writeCanonical(sink, state, false) ? std::optional{sink.finish()} : std::nullopt;
}

std::optional<EnemyMissiles::Checkpoint>
EnemyMissilesCheckpoint::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    try {
        ByteReader reader(bytes);
        if (reader.readU32() != kMagic || reader.readU32() != kVersion) {
            return std::nullopt;
        }
        EnemyMissiles::Checkpoint state;
        state.nextInstance = wide(reader);
        state.shrink = real(reader);
        state.ricochetIn = real(reader);
        const usize missiles = reader.readU32();
        const usize bursts = reader.readU32();
        const usize hits = reader.readU32();
        const usize rocks = reader.readU32();
        const usize pickups = reader.readU32();
        usize events = 0;
        if (missiles > kMaxMissiles || bursts > kMaxBursts || !count(hits, events, kMaxEvents) ||
            !count(rocks, events, kMaxEvents) || !count(pickups, events, kMaxEvents)) {
            return std::nullopt;
        }
        const auto random = RandomCheckpoint::decode(reader.readBytes(RandomCheckpoint::kBytes));
        if (!random || reader.remaining() < missiles * kFlightBytes + bursts * kBurstBytes +
                                                hits * kHitBytes + rocks * kRockBytes +
                                                pickups * kPickupBytes) {
            return std::nullopt;
        }
        state.random = *random;
        usize contacts = 0;
        usize stages = 0;
        state.missiles.reserve(missiles);
        for (usize index = 0; index < missiles; ++index) {
            EnemyMissiles::Checkpoint::Flight flight;
            flight.instance = wide(reader);
            flight.continuity = reader.readU32();
            flight.position = vector(reader);
            flight.velocity = vector(reader);
            flight.turned = vector(reader);
            flight.kind.slot = reader.readS32();
            flight.kind.flags = reader.readU32();
            flight.kind.damage = real(reader);
            flight.kind.speed = real(reader);
            flight.kind.radius = real(reader);
            flight.kind.burstRadius = real(reader);
            flight.kind.spin = vector(reader);
            flight.kind.weight = real(reader);
            flight.kind.held = real(reader);
            flight.resource = reader.readU32();
            flight.shooter = reader.readS32();
            flight.secondsLeft = real(reader);
            flight.lived = real(reader);
            flight.heldLeft = real(reader);
            flight.scale = real(reader);
            const u32 flags = reader.readU32();
            const usize pierced = reader.readU32();
            if ((flags & ~3U) != 0 || pierced > 4 || !count(pierced, contacts, kMaxContacts) ||
                reader.remaining() < pierced * 8) {
                return std::nullopt;
            }
            flight.kind.throughWorld = (flags & 1U) != 0;
            flight.reflected = (flags & 2U) != 0;
            flight.pierced.reserve(pierced);
            for (usize entry = 0; entry < pierced; ++entry) {
                const s32 player = reader.readS32();
                flight.pierced.emplace_back(player, real(reader));
            }
            state.missiles.push_back(std::move(flight));
        }
        state.bursts.reserve(bursts);
        for (usize index = 0; index < bursts; ++index) {
            EnemyMissiles::Burst burst;
            burst.blast.position = vector(reader);
            burst.blast.radius = real(reader);
            burst.blast.damage = real(reader);
            burst.blast.flags = reader.readU32();
            burst.blast.spared = reader.readS32();
            burst.stage = reader.readU32();
            burst.stageLeft = real(reader);
            const u32 flags = reader.readU32();
            const usize stageCount = reader.readU32();
            const usize players = reader.readU32();
            const usize swarm = reader.readU32();
            const usize scenery = reader.readU32();
            if (flags > 1 || stageCount == 0 || players > 4 ||
                !count(stageCount, stages, kMaxStages) || !count(players, contacts, kMaxContacts) ||
                !count(swarm, contacts, kMaxContacts) || !count(scenery, contacts, kMaxContacts) ||
                reader.remaining() < stageCount * 4 + (players + swarm) * 8 + scenery * 4) {
                return std::nullopt;
            }
            burst.blast.lit = flags != 0;
            burst.blast.stages.reserve(stageCount);
            for (usize entry = 0; entry < stageCount; ++entry) {
                burst.blast.stages.push_back(real(reader));
            }
            const auto held = [&](std::vector<EnemyMissiles::Held>& output, usize amount) {
                output.reserve(amount);
                for (usize entry = 0; entry < amount; ++entry) {
                    const s32 id = reader.readS32();
                    output.push_back({id, real(reader)});
                }
            };
            held(burst.players, players);
            held(burst.swarm, swarm);
            burst.scenery.reserve(scenery);
            for (usize entry = 0; entry < scenery; ++entry) {
                burst.scenery.push_back(reader.readS32());
            }
            state.bursts.push_back(std::move(burst));
        }
        state.hits.reserve(hits);
        for (usize index = 0; index < hits; ++index) {
            EnemyMissileHit hit;
            hit.player = reader.readS32();
            hit.shooter = reader.readS32();
            hit.damage = real(reader);
            hit.flags = reader.readU32();
            hit.burstRadius = real(reader);
            hit.position = vector(reader);
            hit.direction = vector(reader);
            const u32 flags = reader.readU32();
            if ((flags & ~15U) != 0) {
                return std::nullopt;
            }
            hit.worldContact = (flags & 1U) != 0;
            hit.fromBurst = (flags & 2U) != 0;
            hit.ricochet = (flags & 4U) != 0;
            hit.liquid = (flags & 8U) != 0;
            hit.target = reader.readS32();
            hit.worldObject = reader.readS32();
            state.hits.push_back(hit);
        }
        state.rockHits.reserve(rocks);
        for (usize index = 0; index < rocks; ++index) {
            const usize rock = reader.readU32();
            state.rockHits.push_back({rock, real(reader)});
        }
        state.pickupBlasts.reserve(pickups);
        for (usize index = 0; index < pickups; ++index) {
            PickupBlastReach blast;
            blast.position = vector(reader);
            blast.radius = real(reader);
            blast.damage = real(reader);
            blast.flags = reader.readU32();
            state.pickupBlasts.push_back(blast);
        }
        return reader.atEnd() && state.valid() ? std::optional{std::move(state)} : std::nullopt;
    } catch (const FormatError&) {
        return std::nullopt;
    }
}

} // namespace gdl::game
