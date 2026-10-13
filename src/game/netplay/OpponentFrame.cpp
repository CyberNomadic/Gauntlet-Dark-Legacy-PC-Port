#include "game/netplay/OpponentFrame.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <tuple>
#include <utility>

#include "engine/io/ByteReader.h"

#include "game/config/GameConfig.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDOF");
constexpr u32 kVersion = 1;
constexpr u32 kPlayerFlags = (1U << 12) - 1;
constexpr f32 kCoordinateLimit = 1'000'000;
constexpr f32 kShapeLimit = 10000;

bool bounded(f32 value, f32 low, f32 high) {
    return std::isfinite(value) && value >= low && value <= high;
}
bool vectorValid(const Vec3& value) {
    return bounded(value.x, -kCoordinateLimit, kCoordinateLimit) &&
           bounded(value.y, -kCoordinateLimit, kCoordinateLimit) &&
           bounded(value.z, -kCoordinateLimit, kCoordinateLimit);
}
bool obstacleValid(const Obstacle& value) {
    return vectorValid(value.centre) && bounded(value.yaw, -kTwoPi, kTwoPi) &&
           bounded(value.halfAcross, 0, kShapeLimit) && bounded(value.halfAlong, 0, kShapeLimit) &&
           bounded(value.height, 0, kShapeLimit) && bounded(value.cylinderRadius, 0, kShapeLimit) &&
           (!value.enemyItem || bounded(value.enemyItem->radius, 0, kShapeLimit));
}
bool viewValid(const EnemyView& value) {
    return value.player >= 0 && value.player < static_cast<s32>(OpponentFrame::kMaxPlayers) &&
           vectorValid(value.position) && bounded(value.radius, 0.001f, 100) &&
           bounded(value.height, 0.001f, 100) && value.level >= 1 && value.level <= 99 &&
           value.meleeWard >= EnemyMeleeWard::None &&
           value.meleeWard <= EnemyMeleeWard::HealthVamp &&
           (!value.collisionHeight || bounded(*value.collisionHeight, 0, 100)) &&
           (!value.collisionRadius || bounded(*value.collisionRadius, 0.001f, 100)) &&
           (!value.decoy || vectorValid(*value.decoy)) && value.heldDeath >= -1 &&
           value.heldDeath < Enemies::kMost;
}
bool obstacleEqual(const Obstacle& left, const Obstacle& right) {
    return left.centre == right.centre && left.yaw == right.yaw &&
           left.halfAcross == right.halfAcross && left.halfAlong == right.halfAlong &&
           left.height == right.height && left.solid == right.solid &&
           left.cylinderRadius == right.cylinderRadius &&
           left.enemyItem.has_value() == right.enemyItem.has_value() &&
           (!left.enemyItem || (left.enemyItem->cylindrical == right.enemyItem->cylindrical &&
                                left.enemyItem->radius == right.enemyItem->radius));
}
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
void obstacle(std::vector<u8>& bytes, const Obstacle& value) {
    vector(bytes, value.centre);
    real(bytes, value.yaw);
    real(bytes, value.halfAcross);
    real(bytes, value.halfAlong);
    real(bytes, value.height);
    real(bytes, value.cylinderRadius);
    real(bytes, value.enemyItem ? value.enemyItem->radius : 0);
    word(bytes, (value.solid ? 1U : 0U) | (value.enemyItem ? 2U : 0U) |
                    (value.enemyItem && value.enemyItem->cylindrical ? 4U : 0U));
}
std::optional<Obstacle> obstacle(ByteReader& reader) {
    Obstacle value;
    value.centre = vector(reader);
    value.yaw = real(reader);
    value.halfAcross = real(reader);
    value.halfAlong = real(reader);
    value.height = real(reader);
    value.cylinderRadius = real(reader);
    const f32 radius = real(reader);
    const u32 flags = reader.readU32();
    if (flags > 7 || ((flags & 2U) == 0 && (radius != 0 || (flags & 4U) != 0))) {
        return std::nullopt;
    }
    value.solid = (flags & 1U) != 0;
    if ((flags & 2U) != 0) {
        value.enemyItem = Obstacle::ItemQuery{(flags & 4U) != 0, radius};
    }
    return value;
}
void player(std::vector<u8>& bytes, const OpponentFrame::Player& player) {
    const auto& value = player.view;
    word(bytes, player.seat);
    integer(bytes, value.player);
    vector(bytes, value.position);
    real(bytes, value.radius);
    real(bytes, value.height);
    integer(bytes, value.level);
    word(bytes, (value.hidden ? 1U : 0U) | (value.captured ? 2U : 0U) |
                    (value.invisible ? 4U : 0U) | (value.antiDeath ? 8U : 0U) |
                    (value.reflects ? 16U : 0U) | (value.it ? 32U : 0U) |
                    (value.damageable ? 64U : 0U) | (value.recentlyHit ? 128U : 0U) |
                    (value.blockableAttack ? 256U : 0U) | (value.collisionHeight ? 512U : 0U) |
                    (value.decoy ? 1024U : 0U) | (value.collisionRadius ? 2048U : 0U));
    word(bytes, static_cast<u32>(value.meleeWard));
    real(bytes, value.collisionHeight.value_or(0));
    vector(bytes, value.decoy.value_or(Vec3{0}));
    real(bytes, value.collisionRadius.value_or(0));
    integer(bytes, value.heldDeath);
}
std::optional<OpponentFrame::Player> player(ByteReader& reader) {
    OpponentFrame::Player player;
    auto& value = player.view;
    const u32 seat = reader.readU32();
    value.player = integer(reader);
    value.position = vector(reader);
    value.radius = real(reader);
    value.height = real(reader);
    value.level = integer(reader);
    const u32 flags = reader.readU32();
    const u32 ward = reader.readU32();
    const f32 height = real(reader);
    const Vec3 decoy = vector(reader);
    const f32 radius = real(reader);
    value.heldDeath = integer(reader);
    if (seat >= OpponentFrame::kMaxPlayers || flags > kPlayerFlags ||
        ward > static_cast<u32>(EnemyMeleeWard::HealthVamp) ||
        ((flags & 512U) == 0 && height != 0) || ((flags & 1024U) == 0 && decoy != Vec3{0}) ||
        ((flags & 2048U) == 0 && radius != 0)) {
        return std::nullopt;
    }
    player.seat = static_cast<u8>(seat);
    value.hidden = (flags & 1U) != 0;
    value.captured = (flags & 2U) != 0;
    value.invisible = (flags & 4U) != 0;
    value.antiDeath = (flags & 8U) != 0;
    value.reflects = (flags & 16U) != 0;
    value.it = (flags & 32U) != 0;
    value.damageable = (flags & 64U) != 0;
    value.recentlyHit = (flags & 128U) != 0;
    value.blockableAttack = (flags & 256U) != 0;
    value.meleeWard = static_cast<EnemyMeleeWard>(ward);
    if ((flags & 512U) != 0) {
        value.collisionHeight = height;
    }
    if ((flags & 1024U) != 0) {
        value.decoy = decoy;
    }
    if ((flags & 2048U) != 0) {
        value.collisionRadius = radius;
    }
    return player;
}
} // namespace

bool OpponentFrame::Player::operator==(const Player& other) const {
    const auto fields = [](const EnemyView& value) {
        return std::tie(value.player, value.position, value.radius, value.height, value.level,
                        value.hidden, value.captured, value.invisible, value.meleeWard,
                        value.antiDeath, value.reflects, value.it, value.damageable,
                        value.recentlyHit, value.collisionHeight, value.blockableAttack,
                        value.decoy, value.collisionRadius, value.heldDeath);
    };
    return seat == other.seat && fields(view) == fields(other.view);
}

bool OpponentFrame::valid() const {
    if (ticks < 1 || ticks > 2 || !std::isfinite(seconds) ||
        std::abs(seconds * 60 - static_cast<f32>(ticks)) > 0.0001f ||
        !bounded(missileSpeed, 0, 16) ||
        std::ranges::find(DifficultyConfig::kGains, difficultyGain) ==
            DifficultyConfig::kGains.end() ||
        initialPlayers < 1 || initialPlayers > kMaxPlayers ||
        pendingPlacements > kMaxPendingPlacements || players.size() > kMaxPlayers ||
        fixtures.size() > kMaxFixtures || stops.size() > kMaxStops ||
        !std::ranges::all_of(fixtures, obstacleValid)) {
        return false;
    }
    u32 seats = 0;
    for (const auto& player : players) {
        if (player.seat >= kMaxPlayers || player.view.player != player.seat ||
            !viewValid(player.view) || (seats & (1U << player.seat)) != 0) {
            return false;
        }
        seats |= 1U << player.seat;
    }
    return std::ranges::all_of(stops, [](const auto& stop) {
        return obstacleValid(stop.box) && stop.rock >= -1 && stop.rock <= 65535 &&
               stop.rockHealth >= 0 && stop.rockArmor >= 0;
    });
}

bool OpponentFrame::operator==(const OpponentFrame& other) const {
    return ticks == other.ticks && seconds == other.seconds && missileSpeed == other.missileSpeed &&
           difficultyGain == other.difficultyGain && initialPlayers == other.initialPlayers &&
           pendingPlacements == other.pendingPlacements && held == other.held &&
           timeStopped == other.timeStopped &&
           unsupportedCombatants == other.unsupportedCombatants && players == other.players &&
           std::ranges::equal(fixtures, other.fixtures, obstacleEqual) &&
           std::ranges::equal(stops, other.stops, [](const auto& left, const auto& right) {
               return obstacleEqual(left.box, right.box) && left.rock == right.rock &&
                      left.rockHealth == right.rockHealth && left.rockArmor == right.rockArmor;
           });
}

std::optional<std::vector<u8>> OpponentFrameCodec::encode(const OpponentFrame& frame) {
    if (!frame.valid()) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(kHeaderBytes + frame.players.size() * kPlayerBytes +
                  frame.fixtures.size() * kObstacleBytes + frame.stops.size() * kStopBytes);
    word(bytes, kMagic);
    word(bytes, kVersion);
    integer(bytes, frame.ticks);
    real(bytes, frame.seconds);
    real(bytes, frame.missileSpeed);
    real(bytes, frame.difficultyGain);
    word(bytes, frame.initialPlayers);
    word(bytes, (frame.timeStopped ? 1U : 0U) | (frame.held ? 2U : 0U) |
                    (frame.unsupportedCombatants ? 4U : 0U));
    word(bytes, frame.pendingPlacements);
    word(bytes, static_cast<u32>(frame.players.size()));
    word(bytes, static_cast<u32>(frame.fixtures.size()));
    word(bytes, static_cast<u32>(frame.stops.size()));
    for (const auto& view : frame.players) {
        player(bytes, view);
    }
    for (const auto& fixture : frame.fixtures) {
        obstacle(bytes, fixture);
    }
    for (const auto& stop : frame.stops) {
        obstacle(bytes, stop.box);
        integer(bytes, stop.rock);
        integer(bytes, stop.rockHealth);
        integer(bytes, stop.rockArmor);
    }
    return bytes;
}

std::optional<OpponentFrame> OpponentFrameCodec::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    try {
        ByteReader reader{bytes};
        if (reader.readU32() != kMagic || reader.readU32() != kVersion) {
            return std::nullopt;
        }
        OpponentFrame frame;
        frame.ticks = integer(reader);
        frame.seconds = real(reader);
        frame.missileSpeed = real(reader);
        frame.difficultyGain = real(reader);
        frame.initialPlayers = reader.readU32();
        const u32 flags = reader.readU32();
        frame.pendingPlacements = reader.readU32();
        const u32 players = reader.readU32();
        const u32 fixtures = reader.readU32();
        const u32 stops = reader.readU32();
        if (flags > 7 || players > OpponentFrame::kMaxPlayers ||
            fixtures > OpponentFrame::kMaxFixtures || stops > OpponentFrame::kMaxStops ||
            reader.remaining() !=
                players * kPlayerBytes + fixtures * kObstacleBytes + stops * kStopBytes) {
            return std::nullopt;
        }
        frame.timeStopped = (flags & 1U) != 0;
        frame.held = (flags & 2U) != 0;
        frame.unsupportedCombatants = (flags & 4U) != 0;
        frame.players.reserve(players);
        frame.fixtures.reserve(fixtures);
        frame.stops.reserve(stops);
        for (u32 index = 0; index < players; ++index) {
            const auto value = player(reader);
            if (!value) {
                return std::nullopt;
            }
            frame.players.push_back(*value);
        }
        for (u32 index = 0; index < fixtures; ++index) {
            const auto value = obstacle(reader);
            if (!value) {
                return std::nullopt;
            }
            frame.fixtures.push_back(*value);
        }
        for (u32 index = 0; index < stops; ++index) {
            const auto box = obstacle(reader);
            if (!box) {
                return std::nullopt;
            }
            frame.stops.push_back({*box, integer(reader), integer(reader), integer(reader)});
        }
        return reader.atEnd() && frame.valid() ? std::optional{std::move(frame)} : std::nullopt;
    } catch (const FormatError&) {
        return std::nullopt;
    }
}
} // namespace gdl::game
