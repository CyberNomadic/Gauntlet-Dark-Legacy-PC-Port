#include "game/netplay/GeneratorCheckpoint.h"

#include <bit>
#include <utility>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
void integer(std::vector<u8>& bytes, s32 value) {
    word(bytes, std::bit_cast<u32>(value));
}
s32 integer(ByteReader& reader) {
    return std::bit_cast<s32>(reader.readU32());
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
void matrix(std::vector<u8>& bytes, const Mat4& value) {
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            real(bytes, value[column][row]);
        }
    }
}
Mat4 matrix(ByteReader& reader) {
    Mat4 value{1};
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            value[column][row] = real(reader);
        }
    }
    return value;
}
void view(std::vector<u8>& bytes, const ViewVolume& value) {
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
void slot(std::vector<u8>& bytes, const Generators::Slot& value) {
    integer(bytes, value.minPlayers);
    word(bytes, static_cast<u32>(value.presence));
    integer(bytes, value.kind);
    integer(bytes, value.tier);
    integer(bytes, value.algorithm);
    integer(bytes, value.most);
    integer(bytes, value.interval);
    real(bytes, value.health);
    real(bytes, value.threshold);
    real(bytes, value.armor);
    integer(bytes, value.state);
    integer(bytes, value.countdown);
    real(bytes, value.ratio);
    integer(bytes, value.bred);
    integer(bytes, value.living);
    vector(bytes, value.position);
    vector(bytes, value.collisionOffset);
    real(bytes, value.targetRadius);
    real(bytes, value.targetHeight);
    matrix(bytes, value.placement);
    real(bytes, value.yaw);
    vector(bytes, value.direction);
    real(bytes, value.clearance);
    real(bytes, value.patrolClearance);
    real(bytes, value.viewRadius);
    word(bytes, (value.alwaysActive ? 1U : 0U) | (value.boss ? 2U : 0U) |
                    (value.box.solid ? 4U : 0U) | (value.box.enemyItem ? 8U : 0U) |
                    (value.box.enemyItem && value.box.enemyItem->cylindrical ? 16U : 0U));
    vector(bytes, value.box.centre);
    real(bytes, value.box.yaw);
    real(bytes, value.box.halfAcross);
    real(bytes, value.box.halfAlong);
    real(bytes, value.box.height);
    real(bytes, value.box.cylinderRadius);
    real(bytes, value.box.enemyItem ? value.box.enemyItem->radius : 0);
    integer(bytes, value.support);
    matrix(bytes, value.supportLocal);
    vector(bytes, value.boxOffset);
    real(bytes, value.boxYaw);
}
std::optional<Generators::Slot> slot(ByteReader& reader) {
    Generators::Slot value;
    value.minPlayers = integer(reader);
    const u32 presence = reader.readU32();
    if (presence > static_cast<u32>(Generators::Slot::Presence::Suppressed)) {
        return std::nullopt;
    }
    value.presence = static_cast<Generators::Slot::Presence>(presence);
    value.kind = integer(reader);
    value.tier = integer(reader);
    value.algorithm = integer(reader);
    value.most = integer(reader);
    value.interval = integer(reader);
    value.health = real(reader);
    value.threshold = real(reader);
    value.armor = real(reader);
    value.state = integer(reader);
    value.countdown = integer(reader);
    value.ratio = real(reader);
    value.bred = integer(reader);
    value.living = integer(reader);
    value.position = vector(reader);
    value.collisionOffset = vector(reader);
    value.targetRadius = real(reader);
    value.targetHeight = real(reader);
    value.placement = matrix(reader);
    value.yaw = real(reader);
    value.direction = vector(reader);
    value.clearance = real(reader);
    value.patrolClearance = real(reader);
    value.viewRadius = real(reader);
    const u32 flags = reader.readU32();
    if ((flags & ~31U) != 0 || ((flags & 8U) == 0 && (flags & 16U) != 0)) {
        return std::nullopt;
    }
    value.alwaysActive = (flags & 1U) != 0;
    value.boss = (flags & 2U) != 0;
    value.box.solid = (flags & 4U) != 0;
    value.box.centre = vector(reader);
    value.box.yaw = real(reader);
    value.box.halfAcross = real(reader);
    value.box.halfAlong = real(reader);
    value.box.height = real(reader);
    value.box.cylinderRadius = real(reader);
    const u32 itemRadius = reader.readU32();
    if ((flags & 8U) != 0) {
        value.box.enemyItem =
            Obstacle::ItemQuery{(flags & 16U) != 0, std::bit_cast<f32>(itemRadius)};
    } else if (itemRadius != 0) {
        return std::nullopt;
    }
    value.support = integer(reader);
    value.supportLocal = matrix(reader);
    value.boxOffset = vector(reader);
    value.boxYaw = real(reader);
    return value;
}
} // namespace

std::optional<std::vector<u8>> GeneratorCheckpoint::encode(const Generators::Checkpoint& state) {
    if (state.generators.size() > kMaxGenerators || state.roster.size() > kMaxGenerators ||
        state.bodies.size() > kMaxBodies || !state.valid()) {
        return std::nullopt;
    }
    const usize size = kHeaderBytes + (state.view ? kViewBytes : 0) +
                       (state.attention ? kAttentionBytes : 0) + state.bodies.size() * kBodyBytes +
                       (state.roster.size() + state.generators.size()) * kSlotBytes;
    std::vector<u8> bytes;
    bytes.reserve(size);
    word(bytes, kVersion);
    word(bytes, static_cast<u32>(state.roster.size()));
    word(bytes, static_cast<u32>(state.generators.size()));
    word(bytes, static_cast<u32>(state.bodies.size()));
    word(bytes, (state.view ? 1U : 0U) | (state.attention ? 2U : 0U));
    real(bytes, state.scales.health);
    real(bytes, state.scales.rate);
    real(bytes, state.scales.most);
    integer(bytes, state.players);
    word(bytes, state.specialBirth);
    if (state.view) {
        view(bytes, *state.view);
    }
    if (state.attention) {
        vector(bytes, *state.attention);
    }
    for (const auto& body : state.bodies) {
        integer(bytes, body.kind);
        word(bytes, body.textures.frame);
        real(bytes, body.textures.remainder);
        real(bytes, body.textures.advance);
    }
    for (const auto& value : state.roster) {
        slot(bytes, value);
    }
    for (const auto& value : state.generators) {
        slot(bytes, value);
    }
    return bytes.size() == size ? std::optional{std::move(bytes)} : std::nullopt;
}

std::optional<Generators::Checkpoint> GeneratorCheckpoint::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    const u32 version = reader.readU32();
    const usize roster = reader.readU32();
    const usize generators = reader.readU32();
    const usize bodies = reader.readU32();
    const u32 flags = reader.readU32();
    if (version != kVersion || roster != generators || generators > kMaxGenerators ||
        bodies > kMaxBodies || flags > 3) {
        return std::nullopt;
    }
    // Counts are bounded before multiplication/reserve, and all records have
    // fixed widths. No malformed length can cause an unbounded allocation or a
    // ByteReader underflow, including optional-state flag changes.
    const usize size = kHeaderBytes + ((flags & 1U) != 0 ? kViewBytes : 0) +
                       ((flags & 2U) != 0 ? kAttentionBytes : 0) + bodies * kBodyBytes +
                       (roster + generators) * kSlotBytes;
    if (bytes.size() != size) {
        return std::nullopt;
    }
    Generators::Checkpoint state;
    state.scales = {real(reader), real(reader), real(reader)};
    state.players = integer(reader);
    state.specialBirth = reader.readU32();
    if ((flags & 1U) != 0) {
        state.view = view(reader);
    }
    if ((flags & 2U) != 0) {
        state.attention = vector(reader);
    }
    state.bodies.reserve(bodies);
    for (usize index = 0; index < bodies; ++index) {
        state.bodies.push_back({integer(reader), {reader.readU32(), real(reader), real(reader)}});
    }
    const auto slots = [&](std::vector<Generators::Slot>& output, usize count) {
        output.reserve(count);
        for (usize index = 0; index < count; ++index) {
            const auto value = slot(reader);
            if (!value) {
                return false;
            }
            output.push_back(*value);
        }
        return true;
    };
    if (!slots(state.roster, roster) || !slots(state.generators, generators) || !reader.atEnd() ||
        !state.valid()) {
        return std::nullopt;
    }
    return state;
}
} // namespace gdl::game
