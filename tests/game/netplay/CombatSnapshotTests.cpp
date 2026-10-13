#include <algorithm>
#include <bit>
#include <cmath>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "game/combat/DamageTypes.h"
#include "game/netplay/CombatReplica.h"
#include "game/netplay/OpponentCheckpoint.h"
#include "game/players/PlayerActor.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Admission = CombatReplica::Admission;

CombatSnapshot state(u64 tick = 0, usize count = 25) {
    CombatSnapshot result;
    result.motion.epoch = 9;
    result.motion.tick = tick;
    result.motion.cameraContinuity = 1;
    for (usize seat = 0; seat < InputCommand::kSeats; ++seat) {
        result.motion.players[seat] = SeatMotion{1, 1, {static_cast<f32>(seat), 0, 0}, 0};
        result.players[seat] = PlayerCombatState{
            1000.5f, ReplicaPlayerLife::Standing, false, true, {12, 7, 1, 3.5f, 0.5f}};
    }
    for (usize i = 0; i < count; ++i) {
        EnemyCombatState enemy;
        enemy.instance = i + 1;
        enemy.health = 45;
        enemy.fullHealth = 50;
        enemy.position = {static_cast<f32>(i), 2, -3};
        enemy.animation = {2, 1, 2, 4.5f, 1};
        result.enemies.push_back(enemy);
    }
    return result;
}
CombatSnapshot rollbackShotState() {
    auto result = state(80, 0);
    ProjectileState shot;
    shot.instance = 0x123456789ABCDEF0ULL;
    shot.resource = 7;
    ProjectileState::Rollback rollback;
    rollback.origin = {result.motion.epoch, 75, 12, 1, 0, false};
    rollback.character = 3;
    rollback.flight = {{1.25f, -2.5f, 3.75f}, {4.25f, -5.5f, 6.75f}, 7.5f, 0.25f, 0.75f, 4};
    shot.rollback = rollback;
    result.projectiles.push_back(shot);
    return result;
}
CombatSnapshot particleShotState(bool retiring = false) {
    auto result = rollbackShotState();
    auto& shot = result.projectiles.front();
    shot.source = ProjectileSource::PlayerRider;
    auto& particles = shot.particles.emplace();
    particles.character = shot.rollback->character;
    particles.origin = shot.rollback->origin;
    particles.flight.birth = {{1.25f, -2.5f, 3.75f}, {4.25f, -5.5f, 6.75f}, 0, 0, 0.75f, 1};
    particles.flight.stepSeconds = 1.0f / 60;
    particles.flight.activeSteps = 15;
    particles.flight.totalSteps = retiring ? 20 : 15;
    particles.flight.retiring = retiring;
    if (retiring) {
        shot.rollback.reset();
    }
    return result;
}
CombatSnapshot particlePotionState(bool retiring = false) {
    auto result = particleShotState(retiring);
    auto& shot = result.projectiles.front();
    shot.source = ProjectileSource::PlayerEffect;
    auto& particles = *shot.particles;
    const auto weapon = *particles.origin;
    particles.character = 0;
    particles.potion = 3;
    particles.potionOrigin = PotionIdentity{weapon.epoch, weapon.tick, weapon.generation,
                                            weapon.grant, weapon.seat, PotionIdentity::Kind::Throw};
    particles.origin.reset();
    if (!retiring) {
        shot.potion = ProjectileState::PotionRollback{*particles.potionOrigin, particles.potion,
                                                      shot.rollback->flight, false};
    }
    shot.rollback.reset();
    return result;
}
CombatSnapshot strikeEffectState() {
    auto result = state(80, 0);
    ProjectileState effect;
    effect.source = ProjectileSource::WorldEffect;
    effect.instance = 4;
    effect.resource = 7;
    effect.effectOrigin = EffectIdentity{result.motion.epoch, 75, 12, 1, 0, 3, 5};
    result.projectiles.push_back(effect);
    return result;
}
CombatSnapshot knockbackState() {
    auto result = state(30, 0);
    auto& player = result.players[0].value();
    player.rollback = PlayerAnimator::Checkpoint{};
    player.knockback = Knockback::Checkpoint{{1.25f, -2.5f, 3.75f},
                                             Knockback::kKnockDown | Knockback::kKnockBack,
                                             17.25f,
                                             {-32, 0, 16},
                                             1.0f / 60,
                                             1.0f / 120,
                                             {2, 0, -3},
                                             true,
                                             true};
    player.reaction = PlayerDeed::FallBack;
    player.knockbackTicks = PlayerCombatState::kKnockbackTicks;
    player.movement.enabled = true;
    player.movement.current = true;
    return result;
}
PlayerMeleeState::Moving movingMelee(const Vec3& feet, usize targetCount = 2,
                                     usize obstacleCount = 1) {
    PlayerMeleeState::Moving result;
    result.origin = feet;
    result.radius = 4;
    result.ticks = PlayerMeleeState::kMaxTicks;
    for (usize index = 0; index < targetCount; ++index) {
        MeleeGeometry::Target target;
        target.id = static_cast<s32>(1000 + index);
        target.instance = index + 1;
        target.base = feet + Vec3{static_cast<f32>(index), 0, 3};
        target.radius = 1;
        target.height = 6;
        target.acquisition = {target.base + Vec3{0, 3, 0}, 1, 1, 10};
        target.contact = true;
        target.contactCell = static_cast<s32>(index);
        result.targets.push_back(target);
    }
    result.obstacles.resize(obstacleCount, Obstacle{feet + Vec3{4, 0, 0}, 0, 2, 3, 5});
    return result;
}
PlayerAnimator::TransitionPose blendPose(usize count = 2, usize nameLength = 4) {
    PlayerAnimator::TransitionPose result;
    result.current.tree.assign(nameLength, 'T');
    for (usize index = 0; index < count; ++index) {
        std::string name(nameLength, 'N');
        const auto suffix = std::to_string(index);
        name.replace(name.size() - suffix.size(), suffix.size(), suffix);
        result.current.nodes.push_back(
            {name, static_cast<s32>(index) - 1, Vec3{static_cast<f32>(index), 1, -2}});
        NodePose pose;
        pose.rotation = {0.125f, -0.25f, 0.5f};
        pose.position = {1.25f, -2.5f, 3.75f};
        pose.scale = {1, 1.5f, -0.75f};
        pose.pitchYawRoll = (index % 2) != 0;
        result.current.poses.push_back(pose);
    }
    result.previous = result.current;
    for (auto& pose : result.previous.poses) {
        pose.rotation *= -1;
        pose.position += Vec3{0.5f, -1, 2};
    }
    return result;
}
PlayerLocomotionState locomotionContacts(const Vec3& origin) {
    PlayerLocomotionState result;
    result.origin = origin;
    result.radius = 4;
    result.ticks = PlayerLocomotionState::kMaxTicks;
    result.bodies = {{origin + Vec3{2, 0, 0}, 1, 3}, {origin + Vec3{3, 0, 0}, 2, 5}};
    result.generators = {Obstacle{origin + Vec3{0, 0, 4}, 0.25f, 1, 2, 3}};
    result.fixtures = {Obstacle{origin + Vec3{0, 0, -4}, -0.5f, 2, 3, 4}};
    result.party = {{origin + Vec3{-2, 0, 0}, 0.75f, 2.5f}};
    return result;
}
std::vector<u8> encode(const CombatSnapshot& snapshot) {
    const auto bytes = CombatPacket::encode(snapshot);
    REQUIRE(bytes);
    return *bytes;
}
CombatReplica::Packets packets(const CombatSnapshot& snapshot) {
    const auto result = CombatReplica::packets(snapshot, SnapshotBlock::Compression::None);
    REQUIRE(result);
    return *result;
}
void writeWord(std::vector<u8>& bytes, usize offset, u32 value) {
    for (usize i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<u8>(value >> (i * 8));
    }
}

OpponentState opponentState() {
    OpponentState result;
    Generators::Slot generator;
    generator.kind = kGruntKind;
    generator.health = 17;
    result.generators.roster.push_back(generator);
    result.generators.generators.push_back(generator);
    result.generators.bodies.push_back({kGruntKind, {13, 0.125f, 0.5f}});
    result.enemies.enemies[0].generator = 0;
    result.enemies.generatorEvents.push_back({0, EnemyGeneratorEvent::Kind::PatrolHit});
    result.enemies.pendingTicks = 1;
    result.enemies.pendingSeconds = 1.0f / 60;
    REQUIRE(result.valid());
    return result;
}

OpponentFrame opponentFrame(const CombatSnapshot& source) {
    OpponentFrame frame;
    for (usize seat = 0; seat < source.players.size(); ++seat) {
        if (!source.players[seat]) {
            continue;
        }
        EnemyView view;
        view.player = static_cast<s32>(seat);
        view.position = source.motion.players[seat]->position;
        frame.players.push_back({static_cast<u8>(seat), view});
    }
    frame.fixtures.push_back({{3, 2, 1}, 0.25f, 1, 2, 3, true});
    frame.stops.push_back({frame.fixtures.front(), -1, 0, 0});
    REQUIRE(frame.valid());
    return frame;
}

TEST_CASE(
    "authored strike effect origins survive snapshot roundtrip independently of missile releases",
    "[netplay][combat-snapshot][effect-identity]") {
    auto source = strikeEffectState();
    for (const u16 occurrence : {u16{0}, u16{127}}) {
        for (const u16 ordinal : {u16{0}, u16{127}}) {
            source.projectiles[0].effectOrigin->occurrence = occurrence;
            source.projectiles[0].effectOrigin->effectOrdinal = ordinal;
            const auto bytes = encode(source);
            const auto decoded = CombatPacket::decode(bytes);
            REQUIRE(decoded);
            CHECK(decoded->projectiles[0].effectOrigin == source.projectiles[0].effectOrigin);
            CHECK_FALSE(decoded->projectiles[0].releaseOrigin());
            CHECK(encode(*decoded) == bytes);
        }
    }
    auto another = source.projectiles.front();
    another.instance++;
    source.projectiles.push_back(another);
    CHECK_FALSE(source.valid()); // same effect must not alias two pool instances
    source.projectiles.back().effectOrigin->occurrence--;
    CHECK(source.valid());
    CHECK(CombatPacket::decode(encode(source)));
}

TEST_CASE("strike effect origins reject foreign authority domains and incompatible payloads",
          "[netplay][combat-snapshot][effect-identity]") {
    const auto source = strikeEffectState();
    for (usize part = 0; part < 10; ++part) {
        auto invalid = source;
        auto& effect = invalid.projectiles.front();
        switch (part) {
        case 0: effect.source = ProjectileSource::PlayerEffect; break;
        case 1: effect.effectOrigin->epoch++; break;
        case 2: effect.effectOrigin->grant++; break;
        case 3: effect.effectOrigin->seat = 4; break;
        case 4: effect.effectOrigin->occurrence = 128; break;
        case 5: effect.effectOrigin->effectOrdinal = 128; break;
        case 6:
            invalid.motion.players[0].reset();
            invalid.players[0].reset();
            break;
        case 7: effect.rollback = rollbackShotState().projectiles[0].rollback; break;
        case 8: effect.particles = particleShotState().projectiles[0].particles; break;
        default: effect.effectOrigin->generation = 0; break;
        }
        CHECK_FALSE(invalid.valid());
        CHECK_FALSE(CombatPacket::encode(invalid));
    }
    auto future = source;
    future.projectiles[0].effectOrigin->tick = future.motion.tick + 1;
    CHECK(future.valid()); // mixed-age local presentation is not a new host checkpoint
    CHECK_FALSE(CombatPacket::encode(future));
}

TEST_CASE("strike effect origin wire blocks are bounded and canonically absent",
          "[netplay][combat-snapshot][effect-identity]") {
    auto source = strikeEffectState();
    constexpr usize kOrigin = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                              InputCommand::kSeats * CombatPacket::kPlayerBytes +
                              CombatPacket::kProjectileBytes - CombatPacket::kEffectOriginBytes -
                              CombatPacket::kPotionRollbackBytes;
    const auto bytes = encode(source);
    for (const auto [offset, value] : std::array<std::pair<usize, u32>, 12>{{{0, 2},
                                                                             {4, 0},
                                                                             {12, 81},
                                                                             {20, 0},
                                                                             {28, 0},
                                                                             {32, 4},
                                                                             {32, 1U << 7U},
                                                                             {32, 128U << 8U},
                                                                             {32, 128U << 16U},
                                                                             {32, 1U << 31U},
                                                                             {4, 10},
                                                                             {28, 2}}}) {
        auto corrupt = bytes;
        writeWord(corrupt, kOrigin + offset, value);
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    source.projectiles[0].effectOrigin.reset();
    const auto absent = encode(source);
    for (usize offset = 0; offset < CombatPacket::kEffectOriginBytes; ++offset) {
        CHECK(absent[kOrigin + offset] == 0);
        auto corrupt = absent;
        corrupt[kOrigin + offset] = 1;
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
}
void commit(CombatReplica& replica, const CombatSnapshot& snapshot) {
    const auto chunks = packets(snapshot);
    for (usize i = 0; i < chunks.size(); ++i) {
        CHECK(replica.receive(1, chunks[i]) ==
              (i + 1 == chunks.size() ? Admission::Committed : Admission::Pending));
    }
    REQUIRE(replica.latest());
    CHECK(encode(*replica.latest()) == encode(snapshot));
}

TEST_CASE("combat payload round trips every seat mask and maximum enemy roster",
          "[netplay][combat-snapshot]") {
    for (u32 mask = 0; mask < 16; ++mask) {
        auto snapshot = state(0x0123456789ABCDEFULL, CombatSnapshot::kMaxEnemies);
        usize seats = 0;
        for (usize seat = 0; seat < InputCommand::kSeats; ++seat) {
            if ((mask & (1U << seat)) == 0) {
                snapshot.players[seat].reset();
                snapshot.motion.players[seat].reset();
            } else {
                ++seats;
            }
        }
        const auto bytes = encode(snapshot);
        CHECK(bytes.size() == CombatPacket::kHeaderBytes + MotionPacket::kHeaderBytes +
                                  seats * (CombatPacket::kPlayerBytes + MotionPacket::kSeatBytes) +
                                  snapshot.enemies.size() * CombatPacket::kEnemyBytes);
        CHECK(bytes.size() <= CombatPacket::kMaxBytes);
        CHECK(bytes[0] == 'G');
        CHECK(bytes[3] == 'B');
        CHECK(bytes[4] == 41);
        CHECK(bytes[6] == 128);
        const auto decoded = CombatPacket::decode(bytes);
        REQUIRE(decoded);
        CHECK(encode(*decoded) == bytes);
        const auto chunks = packets(snapshot);
        for (const auto& chunk : chunks) {
            CHECK(chunk.size() <= PacketTransport::kMaxPacketBytes);
        }
    }
}

TEST_CASE("combat codecs reject incomplete nonfinite inconsistent and unbounded state",
          "[netplay][combat-snapshot]") {
    const auto valid = encode(state());
    for (usize size = 0; size < valid.size(); ++size) {
        CHECK_FALSE(CombatPacket::decode(std::span(valid).first(size)));
    }
    auto bytes = valid;
    bytes.push_back(0);
    CHECK_FALSE(CombatPacket::decode(bytes));
    constexpr usize kPlayer = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes;
    constexpr usize kEnemy = kPlayer + 4 * CombatPacket::kPlayerBytes;
    for (const usize offset : std::array<usize, 6>{0, 4, 6, 8, kPlayer + 4, kEnemy + 20}) {
        bytes = valid;
        bytes[offset] = 255;
        CHECK_FALSE(CombatPacket::decode(bytes));
    }
    for (const usize offset :
         {kPlayer, kPlayer + 24, kPlayer + 28, kEnemy + 24, kEnemy + 28, kEnemy + 32, kEnemy + 36,
          kEnemy + 40, kEnemy + 44, kEnemy + 64, kEnemy + 68}) {
        bytes = valid;
        writeWord(bytes, offset, 0x7FC00000);
        CHECK_FALSE(CombatPacket::decode(bytes));
    }
    for (const u32 flags : {3U, 4096U, 0x80000000U}) {
        bytes = valid;
        writeWord(bytes, kPlayer + 4, flags);
        CHECK_FALSE(CombatPacket::decode(bytes));
    }
    auto invalid = state();
    invalid.players[0].reset();
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = state();
    invalid.enemies[1].instance = invalid.enemies[0].instance;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = state();
    std::swap(invalid.enemies[0], invalid.enemies[1]);
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = state();
    invalid.enemies[0].tier = 0;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = state();
    invalid.enemies[0].animation.generation = 0;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = state();
    invalid.enemies[0].health = -1;
    CHECK_FALSE(CombatReplica::packets(invalid));
    CHECK_FALSE(CombatPacket::encode(state(0, CombatSnapshot::kMaxEnemies + 1)));
}

TEST_CASE("combat supplemental replay payloads preserve independent contacts and exact blend poses",
          "[netplay][combat-snapshot][replay-supplement]") {
    for (u32 mask = 1; mask < 16; ++mask) {
        auto input = state(25, 0);
        for (usize seat = 0; seat < input.players.size(); ++seat) {
            if ((mask & (1U << seat)) == 0) {
                continue;
            }
            auto& player = *input.players[seat];
            player.rollback.emplace();
            player.movement.enabled = true;
            player.movement.current = true;
            if (seat != 2) {
                player.locomotion = locomotionContacts(input.motion.players[seat]->position);
                player.locomotion->automatic = seat == 3; // unknown is distinct from empty
            }
            if (seat != 1) {
                player.rollback->animation.transitionLength = 0.125f;
                player.rollback->animation.transitionTime = 0.03125f;
                player.transitionPose = blendPose();
            }
        }
        const auto bytes = encode(input);
        const auto decoded = CombatPacket::decode(bytes);
        REQUIRE(decoded);
        CHECK(encode(*decoded) == bytes);
        for (usize seat = 0; seat < input.players.size(); ++seat) {
            CAPTURE(mask, seat);
            const auto& source = *input.players[seat];
            const auto& restored = *decoded->players[seat];
            CHECK(restored.transitionPose == source.transitionPose);
            CHECK(restored.locomotion.has_value() == source.locomotion.has_value());
            if (source.locomotion) {
                REQUIRE(restored.locomotion);
                CHECK(PlayerLocomotionCheckpoint::encode(*restored.locomotion) ==
                      PlayerLocomotionCheckpoint::encode(*source.locomotion));
            }
        }
    }
}

TEST_CASE("complete opponent correction shares the atomic motion tick and existing replay section",
          "[netplay][combat-snapshot][opponent-supplement]") {
    for (u32 mask = 0; mask < 16; ++mask) {
        auto source = state(0x123456789ABCDEF0ULL, 0);
        source.opponents = opponentState();
        source.opponentFrame = opponentFrame(source);
        for (usize seat = 0; seat < source.players.size(); ++seat) {
            if ((mask & (1U << seat)) == 0) {
                continue;
            }
            auto& player = *source.players[seat];
            player.rollback.emplace();
            player.movement.enabled = true;
            player.movement.current = true;
            player.locomotion = locomotionContacts(source.motion.players[seat]->position);
            player.rollback->animation.transitionLength = 0.125f;
            player.rollback->animation.transitionTime = 0.03125f;
            player.transitionPose = blendPose();
        }
        const auto bytes = encode(source);
        const auto decoded = CombatPacket::decode(bytes);
        REQUIRE(decoded);
        CHECK(decoded->opponents == source.opponents);
        CHECK(decoded->opponentFrame == source.opponentFrame);
        CHECK(decoded->motion.epoch == source.motion.epoch);
        CHECK(decoded->motion.tick == source.motion.tick);
        CHECK(encode(*decoded) == bytes);
        for (usize seat = 0; seat < source.players.size(); ++seat) {
            CHECK(decoded->players[seat]->transitionPose == source.players[seat]->transitionPose);
        }
        const auto without = [&] {
            auto value = source;
            value.opponents.reset();
            value.opponentFrame.reset();
            return encode(value);
        }();
        const auto opponentBytes = OpponentCheckpoint::encode(*source.opponents);
        const auto frameBytes = OpponentFrameCodec::encode(*source.opponentFrame);
        REQUIRE(opponentBytes);
        REQUIRE(frameBytes);
        CHECK(bytes.size() == without.size() + opponentBytes->size() + frameBytes->size() +
                                  CombatPacket::kOpponentHeaderBytes + (mask == 0 ? 4 : 0));
        const auto absent = CombatPacket::decode(without);
        REQUIRE(absent);
        CHECK_FALSE(absent->opponents);
        CHECK_FALSE(absent->opponentFrame);
    }
}

TEST_CASE("opponent supplements reject cross tick epochs malformed pools and partial prefixes",
          "[netplay][combat-snapshot][opponent-supplement]") {
    auto source = state(55, 0);
    const auto baseline = encode(source);
    source.opponents = opponentState();
    source.opponentFrame = opponentFrame(source);
    const auto bytes = encode(source);
    const usize supplement = baseline.size();
    const usize record = supplement + 4;
    const usize payload = record + CombatPacket::kOpponentHeaderBytes;
    const auto stateBytes = OpponentCheckpoint::encode(*source.opponents);
    REQUIRE(stateBytes);
    const usize frame = payload + stateBytes->size();
    for (usize length = supplement + 1; length < bytes.size(); ++length) {
        auto truncated = bytes;
        truncated.resize(length);
        writeWord(truncated, 60, static_cast<u32>(length - supplement));
        CHECK_FALSE(CombatPacket::decode(truncated));
    }
    for (const auto [offset, value] : std::array<std::pair<usize, u32>, 21>{
             {{supplement, 0},
              {supplement, 32},
              {supplement, 1},
              {record, 0},
              {record, static_cast<u32>(OpponentCheckpoint::kHeaderBytes - 1)},
              {record, static_cast<u32>(OpponentCheckpoint::kMaxBytes + 1)},
              {record + 4, 0},
              {record + 4, static_cast<u32>(OpponentFrameCodec::kHeaderBytes - 1)},
              {record + 4, static_cast<u32>(OpponentFrameCodec::kMaxBytes + 1)},
              {record + 8, 8},   // another epoch
              {record + 12, 1},  // high epoch bits also matter
              {record + 16, 54}, // earlier tick, even if it is a valid older checkpoint
              {record + 16, 56}, // a future simulation seed
              {record + 20, 1},
              {payload, 0},
              {payload + 4, 0},
              {payload + 8, 0xFFFFFFFF},
              {frame, 0},
              {frame + 24, 2}, // valid frame count disagrees with captured pool scales
              {frame + 36, 5},
              {frame + 48, 4}}}) {
        CAPTURE(offset, value);
        auto corrupt = bytes;
        writeWord(corrupt, offset, value);
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    auto trailing = bytes;
    trailing.push_back(0);
    writeWord(trailing, 60, static_cast<u32>(trailing.size() - supplement));
    CHECK_FALSE(CombatPacket::decode(trailing));
    auto hidden = bytes;
    writeWord(hidden, 60, 0);
    CHECK_FALSE(CombatPacket::decode(hidden));
    auto oldVersion = bytes;
    oldVersion[4] = 40;
    CHECK_FALSE(CombatPacket::decode(oldVersion));

    auto invalid = source;
    invalid.opponents->enemies.enemies[0].generator = 1;
    REQUIRE(invalid.opponents->enemies.valid());
    CHECK_FALSE(invalid.valid());
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    invalid.opponents->enemies.blows.resize(EnemyPoolCheckpoint::kMaxEvents + 1);
    REQUIRE(invalid.valid()); // local state is valid but not a transportable envelope
    CHECK_FALSE(CombatPacket::encode(invalid));
    CHECK_FALSE(CombatReplica::packets(invalid));
}

TEST_CASE("fragment loss never publishes opponent pools from a different motion checkpoint",
          "[netplay][combat-snapshot][opponent-supplement]") {
    for (const auto compression :
         {SnapshotBlock::Compression::None, SnapshotBlock::Compression::Automatic}) {
        auto previous = state(40, 0);
        previous.opponents = opponentState();
        previous.opponentFrame = opponentFrame(previous);
        auto next = previous;
        next.motion.tick = 41;
        next.motion.players[0]->position.x = 7;
        next.opponents->generators.generators[0].health = 9;
        next.opponents->enemies.frame = 29;
        next.opponentFrame->players[0].view.position.x = 7;
        CombatReplica receiver;
        REQUIRE(receiver.begin(1, 9));
        commit(receiver, previous);
        const auto chunks = CombatReplica::packets(next, compression);
        REQUIRE(chunks);
        REQUIRE(chunks->size() > 1);
        for (usize index = chunks->size(); index-- > 1;) {
            CHECK(receiver.receive(1, (*chunks)[index]) == Admission::Pending);
            REQUIRE(receiver.latest());
            CHECK(receiver.latest()->motion.tick == previous.motion.tick);
            CHECK(receiver.latest()->opponents == previous.opponents);
            CHECK(receiver.latest()->opponentFrame == previous.opponentFrame);
        }
        CHECK(receiver.receive(1, chunks->front()) == Admission::Committed);
        REQUIRE(receiver.latest());
        CHECK(receiver.latest()->motion.tick == next.motion.tick);
        CHECK(receiver.latest()->motion.players[0]->position == next.motion.players[0]->position);
        CHECK(receiver.latest()->opponents == next.opponents);
        CHECK(receiver.latest()->opponentFrame == next.opponentFrame);

        next.motion.tick = 42;
        next.opponents.reset();
        next.opponentFrame.reset();
        commit(receiver, next);
        CHECK_FALSE(receiver.latest()->opponents); // absence revokes, never retains the old seed
        CHECK_FALSE(receiver.latest()->opponentFrame);
    }
}

TEST_CASE("opponent pools require matching observations and present player seats",
          "[netplay][combat-snapshot][opponent-supplement]") {
    auto source = state(77, 0);
    source.opponents = opponentState();
    source.opponentFrame = opponentFrame(source);
    REQUIRE(source.valid());
    for (u32 change = 0; change < 7; ++change) {
        CAPTURE(change);
        auto invalid = source;
        switch (change) {
        case 0: invalid.opponents.reset(); break;
        case 1: invalid.opponentFrame.reset(); break;
        case 2: invalid.opponentFrame->initialPlayers = 2; break;
        case 3: invalid.opponents->enemies.scales.players = 2; break;
        case 4: invalid.opponents->generators.players = 2; break;
        case 5:
            invalid.players[0].reset();
            invalid.motion.players[0].reset();
            break;
        default: invalid.opponentFrame->players[0].view.player = 1; break;
        }
        CHECK_FALSE(invalid.valid());
        CHECK_FALSE(CombatPacket::encode(invalid));
    }
    // Initial party size is the level-load scaling input. It is deliberately
    // independent of the current number of living/observed seats.
    source.opponentFrame->initialPlayers = 3;
    source.opponents->enemies.scales.players = 3;
    source.opponents->generators.players = 3;
    source.opponentFrame->players.resize(1);
    REQUIRE(source.valid());
    CHECK(CombatPacket::decode(encode(source)));

    // A valid inner frame naming an absent seat must also be refused when it
    // arrives as bytes, not only by callers of valid().
    source.players[1].reset();
    source.motion.players[1].reset();
    const auto bytes = encode(source);
    const auto frame = OpponentFrameCodec::encode(*source.opponentFrame);
    REQUIRE(frame);
    auto wrongSeat = bytes;
    const usize player = bytes.size() - frame->size() + OpponentFrameCodec::kHeaderBytes;
    writeWord(wrongSeat, player, 1);
    writeWord(wrongSeat, player + 4, 1);
    CHECK_FALSE(CombatPacket::decode(wrongSeat));
}

TEST_CASE("combat supplemental replay payloads reject forged lengths seats origins and poses",
          "[netplay][combat-snapshot][replay-supplement]") {
    auto input = state(25, 0);
    for (usize seat = 1; seat < input.players.size(); ++seat) {
        input.players[seat].reset();
        input.motion.players[seat].reset();
    }
    auto& player = *input.players[0];
    player.rollback.emplace();
    player.rollback->animation.transitionLength = 0.125f;
    player.rollback->animation.transitionTime = 0.03125f;
    player.movement.enabled = true;
    player.movement.current = true;
    player.locomotion = locomotionContacts(input.motion.players[0]->position);
    player.transitionPose = blendPose();
    const auto contacts = PlayerLocomotionCheckpoint::encode(*player.locomotion);
    const auto pose = PlayerPoseCheckpoint::encode(*player.transitionPose);
    REQUIRE(contacts);
    REQUIRE(pose);
    const auto bytes = encode(input);
    const usize start = bytes.size() - contacts->size() - pose->size() - 20;
    const usize poseStart = start + 20 + contacts->size();
    for (usize length = start + 1; length < bytes.size(); ++length) {
        auto truncated = bytes;
        truncated.resize(length);
        writeWord(truncated, 60, static_cast<u32>(length - start));
        CHECK_FALSE(CombatPacket::decode(truncated));
    }
    auto omitted = bytes;
    omitted.resize(start);
    writeWord(omitted, 60, 0);
    const auto presentationOnly = CombatPacket::decode(omitted);
    REQUIRE(presentationOnly);
    CHECK_FALSE(presentationOnly->players[0]->locomotion);
    CHECK_FALSE(presentationOnly->players[0]->transitionPose);
    for (const auto [offset, value] : std::array<std::pair<usize, u32>, 21>{
             {{60, 1},
              {60, 3},
              {60, static_cast<u32>(CombatPacket::kReplayBytes + 1)},
              {60, 0xFFFFFFFF},
              {start, 0},
              {start, 16},
              {start, 2}, // a valid seat bit, but this party has no seat 1
              {start, 3},
              {start + 4, 0},
              {start + 4, 1},
              {start + 4, static_cast<u32>(PlayerLocomotionCheckpoint::kMaxBytes + 1)},
              {start + 8, 1},
              {start + 8, static_cast<u32>(PlayerPoseCheckpoint::kMaxBytes + 1)},
              {start + 12, 1},
              {start + 12, static_cast<u32>(TurboEffectCheckpoint::kMaxBytes + 1)},
              {start + 16, 1},
              {start + 16, static_cast<u32>(PlayerPotionCheckpoint::kMaxBytes + 1)},
              {start + 20, std::bit_cast<u32>(1.0f)}, // contacts from another feet origin
              {start + 20 + 20, 0xFFFFFFFF},
              {poseStart, 0xFFFFFFFF},
              {poseStart + 4, 0xFFFFFFFF}}}) {
        CAPTURE(offset, value);
        auto invalid = bytes;
        writeWord(invalid, offset, value);
        CHECK_FALSE(CombatPacket::decode(invalid));
    }
    auto trailing = bytes;
    trailing.push_back(0);
    writeWord(trailing, 60, static_cast<u32>(trailing.size() - start));
    CHECK_FALSE(CombatPacket::decode(trailing));
    auto empty = bytes;
    empty.resize(start + 20);
    writeWord(empty, 60, 20);
    writeWord(empty, start + 4, 0);
    writeWord(empty, start + 8, 0);
    writeWord(empty, start + 12, 0);
    writeWord(empty, start + 16, 0);
    CHECK_FALSE(CombatPacket::decode(empty));
    auto incompatible = bytes;
    incompatible[4] = 37;
    CHECK_FALSE(CombatPacket::decode(incompatible));

    auto invalid = input;
    invalid.players[0]->rollback.reset();
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.players[0]->movement.enabled = false;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.players[0]->locomotion->origin.x += 1;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.players[0]->transitionPose->previous.nodes[0].rest.y += 1;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.players[0]->rollback->animation.transitionTime = 0.125f;
    CHECK_FALSE(CombatPacket::encode(invalid));

    // A presentation snapshot may omit the optional replay seed; replay itself
    // must refuse an active blend without its exact source and current poses.
    player.transitionPose.reset();
    const auto withoutPose = CombatPacket::decode(encode(input));
    REQUIRE(withoutPose);
    REQUIRE(withoutPose->players[0]->rollback);
    CHECK_FALSE(withoutPose->players[0]->transitionPose);
    CHECK(withoutPose->players[0]->rollback->animation.transitionTime <
          withoutPose->players[0]->rollback->animation.transitionLength);
}

TEST_CASE("combat supplemental turbo clocks distinguish never created banks from live zero clocks",
          "[netplay][combat-snapshot][replay-supplement][turbo-effect-checkpoint]") {
    for (u32 mask = 0; mask < 4; ++mask) {
        CAPTURE(mask);
        auto input = state(25, 0);
        auto& player = *input.players[0];
        player.rollback.emplace();
        player.movement.enabled = true;
        player.movement.current = true;
        player.turbo.emplace();
        auto& region = player.turboGeometry.emplace();
        region.selection.feet = input.motion.players[0]->position;
        region.selection.reach = 1.75f;
        region.moving = movingMelee(region.selection.feet, 0, 0);
        auto& effects = player.turboEffects.emplace();
        effects.meleeDamage = 12.5f;
        if ((mask & 1U) != 0) {
            effects.classBank = TextureAnimator::Checkpoint{37, 0.5f, 0.25f};
        }
        if ((mask & 2U) != 0) {
            effects.weaponBank.emplace();
        }
        const auto effectBytes = TurboEffectCheckpoint::encode(effects);
        REQUIRE(effectBytes);
        const auto bytes = encode(input);
        const auto decoded = CombatPacket::decode(bytes);
        REQUIRE(decoded);
        CHECK(decoded->players[0]->turboEffects == player.turboEffects);
        CHECK_FALSE(decoded->players[0]->locomotion);
        CHECK_FALSE(decoded->players[0]->transitionPose);
        CHECK(encode(*decoded) == bytes);

        const usize start = bytes.size() - effectBytes->size() - 20;
        const usize effectsStart = start + 20;
        for (usize length = start + 1; length < bytes.size(); ++length) {
            auto truncated = bytes;
            truncated.resize(length);
            writeWord(truncated, 60, static_cast<u32>(length - start));
            CHECK_FALSE(CombatPacket::decode(truncated));
        }
        auto omitted = bytes;
        omitted.resize(start);
        writeWord(omitted, 60, 0);
        const auto withoutEffects = CombatPacket::decode(omitted);
        REQUIRE(withoutEffects);
        CHECK_FALSE(withoutEffects->players[0]->turboEffects);
        for (const auto [offset, value] : std::array<std::pair<usize, u32>, 5>{
                 {{start + 12, 0},
                  {start + 12, static_cast<u32>(TurboEffectCheckpoint::kMaxBytes + 1)},
                  {effectsStart, 4},
                  {effectsStart + 4, 0x7FC00000},
                  {effectsStart + 4, std::bit_cast<u32>(-1.0f)}}}) {
            auto invalid = bytes;
            writeWord(invalid, offset, value);
            CHECK_FALSE(CombatPacket::decode(invalid));
        }
        auto invalid = input;
        invalid.players[0]->turbo.reset();
        CHECK_FALSE(CombatPacket::encode(invalid));
        invalid = input;
        invalid.players[0]->turboEffects->meleeDamage = TurboEffectState::kMaxDamage + 1;
        CHECK_FALSE(CombatPacket::encode(invalid));
    }
}

TEST_CASE("combat potion flight retains exact release ownership without weapon or burst authority",
          "[netplay][combat-snapshot][potion-checkpoint]") {
    auto input = state(80, 0);
    ProjectileState shot;
    shot.source = ProjectileSource::PlayerEffect;
    shot.instance = 19;
    shot.resource = 7;
    shot.potion = ProjectileState::PotionRollback{
        {input.motion.epoch, 75, 12, 1, 0, PotionIdentity::Kind::Throw},
        4,
        {{1.25f, -2.5f, 3.75f}, {4.25f, -5.5f, 6.75f}, 7.5f, 0.25f, 1, 4},
        true};
    input.projectiles.push_back(shot);
    const auto bytes = encode(input);
    const auto decoded = CombatPacket::decode(bytes);
    REQUIRE(decoded);
    REQUIRE(decoded->projectiles[0].potion);
    CHECK(decoded->projectiles[0].potion->origin == shot.potion->origin);
    CHECK(decoded->projectiles[0].potion->kind == 4);
    CHECK(decoded->projectiles[0].potion->flight == shot.potion->flight);
    CHECK(decoded->projectiles[0].potion->held);
    CHECK(encode(*decoded) == bytes);
    constexpr usize kPotion = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                              InputCommand::kSeats * CombatPacket::kPlayerBytes +
                              CombatPacket::kProjectileBytes - CombatPacket::kPotionRollbackBytes;
    for (const auto [offset, value] : std::array<std::pair<usize, u32>, 17>{
             {{0, 0},
              {0, 2},
              {0, 4},
              {4, 0},
              {4, 10},
              {12, 81},
              {20, 0},
              {28, 0},
              {28, 2},
              {32, 0},
              {32, 4 | 256U},
              {32, 512U},
              {32, 1U << 31U},
              {36, 0},
              {36, 5},
              {68, std::bit_cast<u32>(PlayerMissiles::kLifeSeconds)},
              {76, 0}}}) {
        CAPTURE(offset, value);
        auto invalid = bytes;
        writeWord(invalid, kPotion + offset, value);
        CHECK_FALSE(CombatPacket::decode(invalid));
    }
    for (usize offset = 40; offset < 76; offset += 4) {
        auto invalid = bytes;
        writeWord(invalid, kPotion + offset, 0x7FC00000);
        CHECK_FALSE(CombatPacket::decode(invalid));
    }
    for (usize end = kPotion; end < kPotion + CombatPacket::kPotionRollbackBytes; ++end) {
        CHECK_FALSE(CombatPacket::decode(std::span{bytes}.first(end)));
    }
    auto invalid = input;
    invalid.projectiles[0].source = ProjectileSource::Player;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.projectiles[0].rollback = rollbackShotState().projectiles[0].rollback;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.projectiles[0].particles = particleShotState().projectiles[0].particles;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.projectiles[0].effectOrigin = strikeEffectState().projectiles[0].effectOrigin;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.players[0].reset();
    invalid.motion.players[0].reset();
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.projectiles.push_back(shot);
    ++invalid.projectiles.back().instance;
    CHECK_FALSE(CombatPacket::encode(invalid)); // one release cannot become two physical bottles
    invalid = input;
    invalid.projectiles[0].potion->origin.tick = input.motion.tick + 1;
    CHECK(invalid.valid()); // presentation may lead; wire authority may not
    CHECK_FALSE(CombatPacket::encode(invalid));

    input.projectiles[0].potion.reset();
    const auto absent = encode(input);
    for (usize offset = 0; offset < CombatPacket::kPotionRollbackBytes; ++offset) {
        auto invalidBytes = absent;
        invalidBytes[kPotion + offset] = 1;
        CHECK_FALSE(CombatPacket::decode(invalidBytes));
    }
}

TEST_CASE("combat potion supplements preserve stack tuning and require their replay context",
          "[netplay][combat-snapshot][replay-supplement][potion-checkpoint]") {
    auto input = state(25, 0);
    auto& player = *input.players[0];
    player.rollback.emplace();
    player.movement.enabled = true;
    player.movement.current = true;
    player.floor = PlayerFloorState{0, 0, std::nullopt};
    player.locomotion = locomotionContacts(input.motion.players[0]->position);
    auto& potion = player.potion.emplace();
    potion.potions = {4, 2, 1, 3};
    potion.powers.fill(20.5f);
    potion.bonuses[2] = damage::kOwnColourBonus;
    potion.powers[2] *= potion.bonuses[2];
    potion.damageFlags = damage::kHeal;
    potion.textureFrame = 15.75f;
    potion.ticks = player.locomotion->ticks;
    const auto contacts = PlayerLocomotionCheckpoint::encode(*player.locomotion);
    const auto potionBytes = PlayerPotionCheckpoint::encode(potion);
    REQUIRE(contacts);
    REQUIRE(potionBytes);
    const auto bytes = encode(input);
    const auto restored = CombatPacket::decode(bytes);
    REQUIRE(restored);
    CHECK(restored->players[0]->potion == player.potion);
    CHECK(encode(*restored) == bytes);

    const usize start = bytes.size() - potionBytes->size() - contacts->size() - 20;
    const usize potionStart = start + 20 + contacts->size();
    for (usize length = start + 1; length < bytes.size(); ++length) {
        auto truncated = bytes;
        truncated.resize(length);
        writeWord(truncated, 60, static_cast<u32>(length - start));
        CHECK_FALSE(CombatPacket::decode(truncated));
    }
    for (const auto [offset, value] : std::array<std::pair<usize, u32>, 9>{
             {{start + 16, 0},
              {start + 16, 1},
              {start + 16, static_cast<u32>(PlayerPotionCheckpoint::kMaxBytes + 1)},
              {start + 16, 0xFFFFFFFF},
              {potionStart, PlayerLocomotionState::kMaxTicks + 1},
              {potionStart + 4, 10},
              {potionStart + 8, 1},
              {potionStart + 12, 0x7FC00000},
              {potionStart + PlayerPotionCheckpoint::kHeaderBytes, 0}}}) {
        CAPTURE(offset, value);
        auto invalid = bytes;
        writeWord(invalid, offset, value);
        CHECK_FALSE(CombatPacket::decode(invalid));
    }
    auto omitted = bytes;
    omitted.resize(potionStart);
    writeWord(omitted, start + 16, 0);
    writeWord(omitted, 60, static_cast<u32>(omitted.size() - start));
    const auto withoutPotion = CombatPacket::decode(omitted);
    REQUIRE(withoutPotion);
    CHECK_FALSE(withoutPotion->players[0]->potion);
    REQUIRE(withoutPotion->players[0]->locomotion);

    auto invalid = input;
    invalid.players[0]->rollback.reset();
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.players[0]->locomotion.reset();
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.players[0]->locomotion->origin.x += 1;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.players[0]->floor.reset();
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.players[0]->floor = PlayerFloorState{-1, 0, std::nullopt};
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.players[0]->floor = PlayerFloorState{0, 0x1000, Mat4{1}};
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.players[0]->reaction = PlayerDeed::Flinch;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.players[0]->movement.enabled = false;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.players[0]->movement.current = false;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.players[0]->modifiers = PlayerWeaponModifiers{powerup::kRapidFire, potion.ticks - 1};
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid.players[0]->modifiers->ticks = potion.ticks;
    REQUIRE(CombatPacket::encode(invalid));
    invalid.players[0]->modifiers->shields = powerup::kReflectShield;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.players[0]->rollback->animation.transitionLength = 0.125f;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid.players[0]->transitionPose = blendPose();
    REQUIRE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.players[0]->rollback->shieldAsked = true;
    CHECK_FALSE(CombatPacket::encode(invalid));

    // Empty is a real post-release stack, not an absent observation. It allows
    // recovery to continue without inventing another bottle.
    player.potion->potions.clear();
    const auto empty = CombatPacket::decode(encode(input));
    REQUIRE(empty);
    REQUIRE(empty->players[0]->potion);
    CHECK(empty->players[0]->potion->potions.empty());
}

TEST_CASE("combat packets retain moving melee context only with matching local replay authority",
          "[netplay][combat-snapshot][rollback][melee-checkpoint]") {
    auto input = state(20, 0);
    auto& player = input.players[0].value();
    player.rollback = PlayerAnimator::Checkpoint{};
    player.movement.enabled = true;
    player.movement.current = true;
    auto& melee = player.melee.emplace();
    melee.selection.feet = input.motion.players[0]->position;
    melee.selection.reach = 1.5f;
    melee.moving = movingMelee(melee.selection.feet);
    const auto bytes = encode(input);
    const auto decoded = CombatPacket::decode(bytes);
    REQUIRE(decoded);
    REQUIRE(decoded->players[0]->melee);
    REQUIRE(decoded->players[0]->melee->moving);
    const auto& restored = *decoded->players[0]->melee->moving;
    CHECK(restored.targets.size() == 2);
    CHECK(restored.obstacles.size() == 1);
    CHECK(restored.targets[1].instance == 2);
    CHECK(restored.origin == input.motion.players[0]->position);
    CHECK(encode(*decoded) == bytes);

    auto invalid = input;
    invalid.players[0]->movement.current = false;
    CHECK_FALSE(invalid.valid());
    invalid = input;
    invalid.players[0]->movement.enabled = false;
    CHECK_FALSE(invalid.valid());
    invalid = input;
    invalid.players[0]->movement.ranged = true;
    CHECK_FALSE(invalid.valid());
    invalid = input;
    invalid.players[0]->weapon = PlayerWeaponState{};
    CHECK_FALSE(invalid.valid());
    auto ordinaryPreview = input;
    ordinaryPreview.players[0]->weapon.emplace().resource = 1;
    const auto ordinaryDecoded = CombatPacket::decode(encode(ordinaryPreview));
    REQUIRE(ordinaryDecoded);
    REQUIRE(ordinaryDecoded->players[0]->weapon);
    CHECK_FALSE(ordinaryDecoded->players[0]->turbo);
    CHECK_FALSE(ordinaryDecoded->players[0]->movement.ranged);
    REQUIRE(ordinaryDecoded->players[0]->melee->moving);
    auto volleyPreview = input;
    auto& preview = *volleyPreview.players[0];
    preview.weapon.emplace().resource = 1;
    preview.turbo.emplace();
    preview.turboGeometry = preview.melee;
    const auto previewBytes = encode(volleyPreview);
    const auto restoredPreview = CombatPacket::decode(previewBytes);
    REQUIRE(restoredPreview);
    REQUIRE(restoredPreview->players[0]->weapon);
    REQUIRE(restoredPreview->players[0]->turbo);
    CHECK_FALSE(restoredPreview->players[0]->movement.ranged);
    CHECK(restoredPreview->players[0]->melee->moving.has_value());
    preview.movement.ranged = true;
    CHECK_FALSE(volleyPreview.valid());
    invalid = input;
    invalid.players[0]->rollback.reset();
    CHECK_FALSE(invalid.valid());
    invalid = input;
    invalid.players[0]->melee->moving->origin.x += 1;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = input;
    invalid.motion.players[0]->position.x += 1;
    CHECK_FALSE(CombatPacket::encode(invalid));

    const auto context = PlayerMeleeCheckpoint::encode(melee);
    REQUIRE(context);
    const usize start = bytes.size() - context->size();
    const usize moving = start + PlayerMeleeCheckpoint::kHeaderBytes;
    const usize body = moving + PlayerMeleeCheckpoint::kMovingHeaderBytes;
    for (const auto [offset, value] : {std::pair<usize, u32>{start + 28, 0},
                                       {moving, 0x7FC00000U},
                                       {moving + 16, PlayerMeleeState::kMaxTicks + 1},
                                       {moving + 20, 65},
                                       {moving + 24, 65},
                                       {body + 56, 32},
                                       {body + 60, 4096}}) {
        auto bad = bytes;
        writeWord(bad, offset, value);
        CHECK_FALSE(CombatPacket::decode(bad));
    }
    auto incompatible = bytes;
    incompatible[4] = 24;
    CHECK_FALSE(CombatPacket::decode(incompatible));
}

TEST_CASE("turbo checkpoints have independent bounded regions without revoking ranged previews",
          "[netplay][combat-snapshot][rollback][turbo-checkpoint]") {
    for (u32 mask = 1; mask < 16; ++mask) {
        auto input = state(20, 0);
        for (usize seat = 0; seat < input.players.size(); ++seat) {
            if ((mask & (1U << seat)) == 0) {
                continue;
            }
            auto& player = *input.players[seat];
            player.rollback = PlayerAnimator::Checkpoint{};
            player.movement.enabled = true;
            player.movement.current = true;
            player.movement.ranged = true;
            player.weapon = PlayerWeaponState{};
            player.weapon->resource = 1;
            player.turbo = PlayerTurboState{};
            player.turbo->meter.held = 100;
            player.turbo->meter.shown = 40;
            player.weaponHidden = true;
            player.shove = (seat % 2) == 0;
            auto& region = player.turboGeometry.emplace();
            region.selection.feet = input.motion.players[seat]->position;
            region.selection.reach = 1.5f;
            region.moving = movingMelee(region.selection.feet, 0, 0);
        }
        const auto bytes = encode(input);
        const auto restored = CombatPacket::decode(bytes);
        REQUIRE(restored);
        CHECK(encode(*restored) == bytes);
        for (usize seat = 0; seat < input.players.size(); ++seat) {
            CHECK(restored->players[seat]->turbo == input.players[seat]->turbo);
            CHECK(restored->players[seat]->shove == input.players[seat]->shove);
            if (input.players[seat]->turbo) {
                CHECK(restored->players[seat]->movement.ranged);
                CHECK(restored->players[seat]->weapon.has_value());
                CHECK(restored->players[seat]->weaponHidden);
                CHECK_FALSE(restored->players[seat]->melee);
            }
        }
        CombatReplica replica;
        REQUIRE(replica.begin(1, input.motion.epoch));
        commit(replica, input);
    }
}

TEST_CASE("turbo packet admission rejects incomplete unsafe or displaced replay context",
          "[netplay][combat-snapshot][rollback][turbo-checkpoint]") {
    auto input = state(20, 0);
    auto& player = *input.players[0];
    player.rollback = PlayerAnimator::Checkpoint{};
    player.movement.enabled = true;
    player.movement.current = true;
    player.turbo = PlayerTurboState{};
    auto& region = player.turboGeometry.emplace();
    region.selection.feet = input.motion.players[0]->position;
    region.selection.reach = 1.5f;
    region.moving = movingMelee(region.selection.feet);
    const auto bytes = encode(input);
    const auto timeline = PlayerTurboCheckpoint::encode(*player.turbo);
    const auto geometry = PlayerMeleeCheckpoint::encode(region);
    REQUIRE(timeline);
    REQUIRE(geometry);
    const usize start = bytes.size() - timeline->size() - geometry->size() - 12;
    for (usize size = start; size < bytes.size(); ++size) {
        CHECK_FALSE(CombatPacket::decode(std::span{bytes}.first(size)));
    }
    for (const auto [offset, value] :
         {std::pair<usize, u32>{52, static_cast<u32>(CombatPacket::kTurboBytes + 1)},
          {start, 0},
          {start, 16},
          {start + 4, static_cast<u32>(PlayerTurboCheckpoint::kMaxBytes + 1)},
          {start + 8, static_cast<u32>(PlayerMeleeCheckpoint::kMaxBytes + 1)},
          {start + 12, 0x7FC00000U},
          {start + 12 + timeline->size(), std::bit_cast<u32>(1.0f)}}) {
        auto bad = bytes;
        writeWord(bad, offset, value);
        CHECK_FALSE(CombatPacket::decode(bad));
    }
    for (u32 variant = 0; variant < 8; ++variant) {
        auto bad = input;
        auto& unsafe = *bad.players[0];
        switch (variant) {
        case 0: unsafe.turbo.reset(); break;
        case 1: unsafe.turboGeometry.reset(); break;
        case 2: unsafe.turboGeometry->moving.reset(); break;
        case 3: unsafe.rollback.reset(); break;
        case 4: unsafe.movement.current = false; break;
        case 5: unsafe.movement.enabled = false; break;
        case 6: unsafe.turbo->meter.held = 101; break;
        case 7: bad.motion.players[0]->position.x += 1; break;
        default: FAIL("Unknown turbo checkpoint mutation");
        }
        CHECK_FALSE(CombatPacket::encode(bad));
    }
    auto old = bytes;
    old[4] = 26;
    CHECK_FALSE(CombatPacket::decode(old));
}

TEST_CASE("shove certificates require a complete native turbo replay context",
          "[netplay][combat-snapshot][shove-prediction]") {
    auto input = state(20, 0);
    auto& player = *input.players[0];
    CHECK_FALSE(player.shove);
    const auto ordinary = encode(input);
    player.shove = true;
    CHECK_FALSE(CombatPacket::encode(input));
    auto malformed = ordinary;
    constexpr usize kFlags = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes + 4;
    writeWord(malformed, kFlags, 2048);
    CHECK_FALSE(CombatPacket::decode(malformed));

    player.rollback = PlayerAnimator::Checkpoint{};
    player.movement.enabled = true;
    player.movement.current = true;
    player.turbo = PlayerTurboState{};
    auto& region = player.turboGeometry.emplace();
    region.selection.feet = input.motion.players[0]->position;
    region.selection.reach = 1.5f;
    region.moving = movingMelee(region.selection.feet, 0, 0);
    REQUIRE(CombatPacket::decode(encode(input))->players[0]->shove);
    for (u32 variant = 0; variant < 7; ++variant) {
        auto bad = input;
        auto& unsafe = *bad.players[0];
        switch (variant) {
        case 0: unsafe.turbo.reset(); break;
        case 1: unsafe.turboGeometry.reset(); break;
        case 2: unsafe.turboGeometry->moving.reset(); break;
        case 3: unsafe.rollback.reset(); break;
        case 4: unsafe.movement.enabled = false; break;
        case 5: unsafe.movement.current = false; break;
        case 6: unsafe.portalPhase = 0.0f; break;
        default: FAIL("Unknown shove checkpoint mutation");
        }
        CHECK_FALSE(CombatPacket::encode(bad));
    }
}

TEST_CASE("combat checkpoint carries native rollback state and only executed input seeds",
          "[netplay][combat-snapshot][rollback]") {
    auto source = state(30, 0);
    auto& player = *source.players[0];
    player.rollback = PlayerAnimator::Checkpoint{};
    player.rollback->animation.playing = true;
    player.rollback->animation.generation = 8;
    player.rollback->animation.time = 0.123456789;
    player.rollback->phaseSeconds = 1.0 / 71.0;
    player.rollback->quickHeld = true;
    player.movement.enabled = true;
    player.movement.ranged = true;
    player.movement.current = true;
    InputCommand executed;
    executed.epoch = source.motion.epoch;
    executed.tick = 28;
    executed.grant = source.motion.players[0]->grant;
    executed.seat = 0;
    executed.heldButtons = static_cast<u32>(CommandHeld::Attack);
    player.rollbackInput = executed;
    const auto bytes = encode(source);
    const auto decoded = CombatPacket::decode(bytes);
    REQUIRE(decoded);
    CHECK(decoded->players[0]->rollback == player.rollback);
    REQUIRE(decoded->players[0]->rollbackInput);
    CHECK(decoded->players[0]->rollbackInput->tick == 28);
    CHECK(decoded->players[0]->rollbackInput->heldButtons == static_cast<u32>(CommandHeld::Attack));
    CHECK(decoded->players[0]->movement.ranged);
    CHECK(decoded->players[0]->movement.current);
    CHECK(encode(*decoded) == bytes);

    std::array invalidSeeds{executed, executed, executed, executed};
    ++invalidSeeds[0].epoch;
    invalidSeeds[1].tick = 31;
    ++invalidSeeds[2].grant;
    invalidSeeds[3].seat = 1;
    for (const auto& invalid : invalidSeeds) {
        player.rollbackInput = invalid;
        CHECK_FALSE(CombatPacket::encode(source));
    }
    player.rollbackInput = executed;
    player.rollback.reset();
    CHECK_FALSE(CombatPacket::encode(source));
    player.movement.ranged = false;
    const auto withoutState = encode(source);
    constexpr usize kRollback = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                                CombatPacket::kPlayerBytes - CombatPacket::kRollbackBytes -
                                CombatPacket::kRollbackInputBytes - CombatPacket::kWeaponBytes -
                                CombatPacket::kKnockbackBytes - CombatPacket::kModifierBytes -
                                CombatPacket::kArmEquipmentBytes;
    auto dirtyPadding = withoutState;
    writeWord(dirtyPadding, kRollback, 1);
    CHECK_FALSE(CombatPacket::decode(dirtyPadding));
    player.rollbackInput.reset();
    dirtyPadding = encode(source);
    writeWord(dirtyPadding, kRollback + CombatPacket::kRollbackBytes, 1);
    CHECK_FALSE(CombatPacket::decode(dirtyPadding));
}

TEST_CASE("latest local presentation permission is independent of replay and rejects world locks",
          "[netplay][combat-snapshot][rollback]") {
    auto source = state(30, 0);
    auto& player = *source.players[0];
    player.movement.current = true;
    REQUIRE_FALSE(player.movement.enabled);
    player.animation.action = static_cast<u32>(PlayerAnimator::Action::Quick1);
    const auto bytes = encode(source);
    const auto decoded = CombatPacket::decode(bytes);
    REQUIRE(decoded);
    CHECK(decoded->players[0]->movement.current);
    CHECK_FALSE(decoded->players[0]->movement.enabled);

    player.portalPhase = 0.5f;
    CHECK_FALSE(CombatPacket::encode(source));
    player.portalPhase.reset();
    player.life = ReplicaPlayerLife::Dying;
    CHECK_FALSE(CombatPacket::encode(source));
    player.movement.current = false;
    CHECK(CombatPacket::encode(source));

    constexpr usize kMovement = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                                CombatPacket::kPlayerBytes - CombatPacket::kMovementBytes -
                                CombatPacket::kRollbackBytes - CombatPacket::kRollbackInputBytes -
                                CombatPacket::kWeaponBytes - CombatPacket::kKnockbackBytes -
                                CombatPacket::kModifierBytes - CombatPacket::kArmEquipmentBytes;
    auto unknownFlag = bytes;
    writeWord(unknownFlag, kMovement, 16);
    CHECK_FALSE(CombatPacket::decode(unknownFlag));
}

TEST_CASE("combat wire preserves host queued impacts and replay budgets independently per seat",
          "[netplay][combat-snapshot][rollback][knockback-checkpoint]") {
    const auto approved = knockbackState().players[0].value();
    const auto plainSize = encode(state(30, 0)).size();
    for (u32 mask = 0; mask < 16; ++mask) {
        auto source = state(30, 0);
        for (usize seat = 0; seat < InputCommand::kSeats; ++seat) {
            if ((mask & (1U << seat)) != 0) {
                source.players[seat] = approved;
                source.players[seat]->knockbackTicks = static_cast<u32>(seat) + 1;
                source.players[seat]->knockback->force.x += static_cast<f32>(seat);
                source.players[seat]->knockback->shoved = (seat & 1U) != 0;
                source.players[seat]->knockback->pushed = (seat & 2U) != 0;
            }
        }
        const auto bytes = encode(source);
        CHECK(bytes.size() == plainSize); // the extension is fixed even when absent
        const auto decoded = CombatPacket::decode(bytes);
        REQUIRE(decoded);
        for (usize seat = 0; seat < InputCommand::kSeats; ++seat) {
            REQUIRE(decoded->players[seat]);
            REQUIRE(source.players[seat]);
            CHECK(decoded->players[seat]->knockback == source.players[seat]->knockback);
            CHECK(decoded->players[seat]->reaction == source.players[seat]->reaction);
            CHECK(decoded->players[seat]->knockbackTicks == source.players[seat]->knockbackTicks);
        }
        CHECK(encode(*decoded) == bytes);
    }
    auto source = knockbackState();
    for (const u32 ticks : {0U, 1U, PlayerCombatState::kKnockbackTicks}) {
        source.players[0]->knockbackTicks = ticks;
        const auto decoded = CombatPacket::decode(encode(source));
        REQUIRE(decoded);
        REQUIRE(decoded->players[0]);
        CHECK(decoded->players[0]->knockbackTicks == ticks);
    }
    // The transport carries any valid pending deed; replay admission separately
    // decides which reactions can be continued without other gameplay state.
    for (u32 value = 0; value <= static_cast<u32>(PlayerDeed::ComboThrown); ++value) {
        source.players[0]->reaction = static_cast<PlayerDeed>(value);
        const auto decoded = CombatPacket::decode(encode(source));
        REQUIRE(decoded);
        REQUIRE(decoded->players[0]);
        CHECK(decoded->players[0]->reaction == static_cast<PlayerDeed>(value));
    }
    source.players[0]->knockback = Knockback::Checkpoint{};
    source.players[0]->reaction = PlayerDeed::None;
    source.players[0]->knockbackTicks = 0;
    const auto empty = CombatPacket::decode(encode(source));
    REQUIRE(empty);
    REQUIRE(empty->players[0]);
    REQUIRE(empty->players[0]->knockback);
    CHECK(*empty->players[0]->knockback == Knockback::Checkpoint{});
}

TEST_CASE("combat impact wire rejects malformed clocks reactions budgets and absent padding",
          "[netplay][combat-snapshot][rollback][knockback-checkpoint]") {
    constexpr usize kPlayer = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes;
    constexpr usize kImpulse = kPlayer + CombatPacket::kPlayerBytes -
                               CombatPacket::kKnockbackBytes - CombatPacket::kModifierBytes -
                               CombatPacket::kArmEquipmentBytes;
    constexpr usize kReaction = kImpulse + PlayerKnockbackCheckpoint::kBytes;
    constexpr usize kBudget = kReaction + 4;
    const auto source = knockbackState();
    const auto bytes = encode(source);
    CHECK((bytes[kPlayer + 5] & 2U) != 0); // player bit512 announces the checkpoint
    for (const usize offset : {0U, 4U, 8U, 16U, 20U, 24U, 28U, 32U, 36U, 40U, 44U, 48U}) {
        for (const u32 invalid : {0x7FC00000U, 0x7F800000U, 0xFF800000U, 0x7F7FFFFFU}) {
            auto changed = bytes;
            writeWord(changed, kImpulse + offset, invalid);
            CHECK_FALSE(CombatPacket::decode(changed));
        }
    }
    for (const u32 reaction : {static_cast<u32>(PlayerDeed::ComboThrown) + 1U, 256U, 0xFFFFFFFFU}) {
        auto changed = bytes;
        writeWord(changed, kReaction, reaction);
        CHECK_FALSE(CombatPacket::decode(changed)); // reject before narrowing the enum
    }
    for (const u32 budget : {PlayerCombatState::kKnockbackTicks + 1U, 0xFFFFFFFFU}) {
        auto changed = bytes;
        writeWord(changed, kBudget, budget);
        CHECK_FALSE(CombatPacket::decode(changed));
    }
    for (const auto [offset, value] : std::array<std::pair<usize, u32>, 4>{
             {{36, std::bit_cast<u32>(1.0f / Knockback::kFrameRate)},
              {32, std::bit_cast<u32>(-0.001f)},
              {44, std::bit_cast<u32>(0.001f)},
              {52, 4}}}) {
        auto changed = bytes;
        writeWord(changed, kImpulse + offset, value);
        CHECK_FALSE(CombatPacket::decode(changed));
    }
    const auto absent = encode(state(30, 0));
    for (usize offset = 0; offset < CombatPacket::kKnockbackBytes; ++offset) {
        auto changed = absent;
        changed[kImpulse + offset] = 1;
        CHECK_FALSE(CombatPacket::decode(changed));
    }
    auto hidden = bytes;
    writeWord(hidden, kPlayer + 4, 8U | 64U); // retained nonzero tail without presence bit
    CHECK_FALSE(CombatPacket::decode(hidden));
    auto oldVersion = bytes;
    oldVersion[4] = 23;
    CHECK_FALSE(CombatPacket::decode(oldVersion));
}

TEST_CASE("combat impact replay permissions require an action checkpoint and isolated movement",
          "[netplay][combat-snapshot][rollback][knockback-checkpoint]") {
    const auto source = knockbackState();
    constexpr usize kPlayer = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes;
    constexpr usize kImpulse = kPlayer + CombatPacket::kPlayerBytes -
                               CombatPacket::kKnockbackBytes - CombatPacket::kModifierBytes -
                               CombatPacket::kArmEquipmentBytes;
    constexpr usize kRollback = kImpulse - CombatPacket::kWeaponBytes -
                                CombatPacket::kRollbackInputBytes - CombatPacket::kRollbackBytes;
    constexpr usize kMovement = kRollback - CombatPacket::kMovementBytes;
    auto invalid = source;
    invalid.players[0]->rollback.reset();
    CHECK_FALSE(CombatPacket::encode(invalid));
    auto missingAction = encode(source);
    writeWord(missingAction, kPlayer + 4, 8U | 512U);
    for (usize offset = 0; offset < CombatPacket::kRollbackBytes; ++offset) {
        missingAction[kRollback + offset] = 0;
    }
    CHECK_FALSE(CombatPacket::decode(missingAction));

    for (const u32 movement : {0U, 1U, 8U}) {
        auto changed = encode(source);
        writeWord(changed, kMovement, movement);
        CHECK_FALSE(CombatPacket::decode(changed));
    }
    invalid = source;
    invalid.players[0]->movement.current = false;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    invalid.players[0]->movement.enabled = false;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    auto& melee = invalid.players[0]->melee.emplace();
    melee.selection.feet = invalid.motion.players[0]->position;
    melee.selection.reach = 1.5f;
    CHECK_FALSE(CombatPacket::encode(invalid));
    // A zero-budget checkpoint is still useful as authoritative state; it does
    // not independently promise that local replay is permitted.
    invalid.players[0]->knockbackTicks = 0;
    const auto noBudget = encode(invalid);
    const auto decoded = CombatPacket::decode(noBudget);
    REQUIRE(decoded);
    REQUIRE(decoded->players[0]);
    CHECK(decoded->players[0]->knockback);
    CHECK(decoded->players[0]->melee);
    CHECK(decoded->players[0]->knockbackTicks == 0);
    auto simultaneous = noBudget;
    writeWord(simultaneous, kImpulse + PlayerKnockbackCheckpoint::kBytes + 4, 1);
    CHECK_FALSE(CombatPacket::decode(simultaneous));

    invalid = source;
    invalid.players[0]->knockback.reset();
    invalid.players[0]->knockbackTicks = 0;
    CHECK_FALSE(CombatPacket::encode(invalid)); // an absent checkpoint cannot queue a reaction
    invalid.players[0]->reaction = PlayerDeed::None;
    invalid.players[0]->knockbackTicks = 1;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid.players[0]->knockbackTicks = 0;
    CHECK(CombatPacket::encode(invalid));
    invalid = source;
    invalid.players[0]->reaction = static_cast<PlayerDeed>(255);
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    invalid.players[0]->knockbackTicks = PlayerCombatState::kKnockbackTicks + 1;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    invalid.players[0]->knockback->frameSeconds = 1.0f / Knockback::kFrameRate;
    CHECK_FALSE(CombatPacket::encode(invalid));
    for (const auto life : {ReplicaPlayerLife::Dying, ReplicaPlayerLife::InTower}) {
        invalid = source;
        invalid.players[0]->life = life;
        CHECK_FALSE(CombatPacket::encode(invalid));
    }
    invalid = source;
    invalid.players[0]->portalPhase = 0.5f;
    CHECK_FALSE(CombatPacket::encode(invalid));
}

TEST_CASE("native weapon descriptors preserve every field without requiring an aim target",
          "[netplay][combat-snapshot][rollback]") {
    auto source = state(40, 0);
    PlayerWeaponState weapon;
    weapon.character = 16;
    weapon.hand = {-1.5f, 2.25f, 3.5f};
    weapon.speed = 42.5f;
    weapon.resource = 65535;
    weapon.streak = 42;
    weapon.tint = Color{0x12, 0x34, 0x56, 0x78};
    weapon.forward = -3.5f;
    weapon.target = Vec3{12.25f, -23.5f, 34.75f};
    weapon.textureFrame = 431.5f;
    for (const bool targeted : {false, true}) {
        source.players[0]->weapon = weapon;
        if (!targeted) {
            source.players[0]->weapon->target.reset();
            source.players[0]->weapon->textureFrame.reset();
        }
        const auto bytes = encode(source);
        const auto decoded = CombatPacket::decode(bytes);
        REQUIRE(decoded);
        REQUIRE(decoded->players[0]);
        REQUIRE(decoded->players[0]->weapon);
        const auto& actual = *decoded->players[0]->weapon;
        CHECK(actual.character == weapon.character);
        CHECK(actual.hand == weapon.hand);
        CHECK(actual.speed == weapon.speed);
        CHECK(actual.resource == weapon.resource);
        CHECK(actual.streak == weapon.streak);
        CHECK(actual.tint == weapon.tint);
        CHECK(actual.forward == weapon.forward);
        CHECK(actual.target == source.players[0]->weapon->target);
        CHECK(actual.textureFrame == source.players[0]->weapon->textureFrame);
        CHECK(actual.shots == 1);
        CHECK_FALSE(decoded->players[1]->weapon);
        CHECK(encode(*decoded) == bytes);
    }
}

TEST_CASE("weapon descriptor codecs reject malformed fields and noncanonical absent data",
          "[netplay][combat-snapshot][rollback]") {
    auto source = state(40, 0);
    PlayerWeaponState weapon;
    weapon.resource = 1;
    weapon.target = Vec3{1, 2, 3};
    weapon.textureFrame = 321.5f;
    source.players[0]->weapon = weapon;
    const auto bytes = encode(source);
    constexpr usize kWeapon = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                              CombatPacket::kPlayerBytes - CombatPacket::kWeaponBytes -
                              CombatPacket::kKnockbackBytes - CombatPacket::kModifierBytes -
                              CombatPacket::kArmEquipmentBytes;
    for (const usize offset : {4U, 8U, 12U, 16U, 32U, 40U, 44U, 48U, 52U}) {
        for (const u32 value : {0x7FC00000U, 0x7F800000U, 0xFF800000U}) {
            auto corrupt = bytes;
            writeWord(corrupt, kWeapon + offset, value);
            CHECK_FALSE(CombatPacket::decode(corrupt));
        }
    }
    for (const auto [offset, value] : std::array<std::pair<usize, u32>, 7>{
             {{0, 17}, {20, 0}, {20, 65536}, {24, 65536}, {36, 8}, {36, 0}, {0, 0xFFFFFFFF}}}) {
        auto corrupt = bytes;
        writeWord(corrupt, kWeapon + offset, value);
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    const std::array<std::pair<usize, f32>, 12> outside{{{4, -100.01f},
                                                         {8, 100.01f},
                                                         {12, 100.01f},
                                                         {16, 19.99f},
                                                         {16, 60.01f},
                                                         {32, -1024.01f},
                                                         {32, 1024.01f},
                                                         {40, -1000001.0f},
                                                         {40, 1000001.0f},
                                                         {44, 1000001.0f},
                                                         {48, -1000001.0f},
                                                         {48, 1000001.0f}}};
    for (const auto [offset, value] : outside) {
        auto corrupt = bytes;
        writeWord(corrupt, kWeapon + offset, std::bit_cast<u32>(value));
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    for (const bool upper : {false, true}) {
        auto& bounded = *source.players[0]->weapon;
        bounded.character = upper ? 16 : 0;
        bounded.hand = Vec3{upper ? 100.0f : -100.0f};
        bounded.speed = upper ? 60.0f : 20.0f;
        bounded.resource = upper ? 65535 : 1;
        bounded.streak = upper ? 65535 : 0;
        bounded.forward = upper ? 1024.0f : -1024.0f;
        bounded.target = Vec3{upper ? 1000000.0f : -1000000.0f};
        bounded.textureFrame = upper ? 1000000000.0f : 0;
        CHECK(CombatPacket::decode(encode(source)));
    }
    source.players[0]->weapon.reset();
    const auto absent = encode(source);
    for (usize offset = 0; offset < CombatPacket::kWeaponBytes; offset += 4) {
        auto corrupt = absent;
        writeWord(corrupt, kWeapon + offset, 1);
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    source.players[0]->weapon = weapon;
    source.players[0]->weapon->target.reset();
    source.players[0]->weapon->textureFrame.reset();
    const auto untargeted = encode(source);
    for (const usize offset : {40U, 44U, 48U, 52U}) {
        auto corrupt = untargeted;
        writeWord(corrupt, kWeapon + offset, 1);
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    source.players[0]->weapon->speed = 19;
    CHECK_FALSE(CombatPacket::encode(source));
    source.players[0]->weapon = weapon;
    for (const f32 clock : {-1.0f, 1000001024.0f}) {
        source.players[0]->weapon->textureFrame = clock;
        CHECK_FALSE(CombatPacket::encode(source));
    }
}

TEST_CASE("weapon modifier certificates and fan sizes round trip independently per seat",
          "[netplay][combat-snapshot][item-rollback]") {
    auto source = state(40, 0);
    constexpr std::array<u32, 7> masks{powerup::kRapidFire,
                                       powerup::kThreeWayShot,
                                       powerup::kFiveWayShot,
                                       powerup::kRapidFire | powerup::kThreeWayShot,
                                       powerup::kRapidFire | powerup::kFiveWayShot,
                                       powerup::kThreeWayShot | powerup::kFiveWayShot,
                                       PlayerMissiles::kPlainModifiers};
    for (const u32 mask : masks) {
        for (const u32 ticks : {0U, 1U, PlayerWeaponModifiers::kMaxTicks}) {
            for (usize seat = 0; seat < InputCommand::kSeats; ++seat) {
                auto& player = *source.players[seat];
                player.rollback = PlayerAnimator::Checkpoint{};
                player.modifiers = PlayerWeaponModifiers{mask, ticks};
                PowerupEffects effects;
                effects.weapon = mask;
                player.weapon = PlayerWeaponState{};
                player.weapon->resource = 1;
                player.weapon->shots = static_cast<u8>(effects.shots());
            }
            const auto bytes = encode(source);
            const auto decoded = CombatPacket::decode(bytes);
            REQUIRE(decoded);
            CHECK(encode(*decoded) == bytes);
            for (usize seat = 0; seat < InputCommand::kSeats; ++seat) {
                REQUIRE(decoded->players[seat]->modifiers);
                CHECK(decoded->players[seat]->modifiers->flags == mask);
                CHECK(decoded->players[seat]->modifiers->ticks == ticks);
                CHECK(decoded->players[seat]->weapon->shots == source.players[seat]->weapon->shots);
            }
        }
    }
}

TEST_CASE("modifier wire rejects unsafe masks budgets mismatched fans and hidden state",
          "[netplay][combat-snapshot][item-rollback]") {
    auto source = state(40, 0);
    auto& player = *source.players[0];
    player.rollback = PlayerAnimator::Checkpoint{};
    player.modifiers = PlayerWeaponModifiers{powerup::kRapidFire | powerup::kFiveWayShot, 12};
    player.weapon = PlayerWeaponState{};
    player.weapon->resource = 1;
    player.weapon->shots = 5;
    const auto bytes = encode(source);
    constexpr usize kPlayer = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes;
    constexpr usize kModifiers = kPlayer + CombatPacket::kPlayerBytes -
                                 CombatPacket::kModifierBytes - CombatPacket::kArmEquipmentBytes;
    constexpr usize kShots =
        kModifiers - CombatPacket::kKnockbackBytes - CombatPacket::kWeaponBytes + 56;
    for (const auto [offset, value] : std::array<std::pair<usize, u32>, 14>{
             {{kModifiers, 0},
              {kModifiers, powerup::kSuperShot},
              {kModifiers, PlayerWeaponModifiers::kFlags | powerup::kThunderHammer},
              {kModifiers + 4, PlayerWeaponModifiers::kMaxTicks + 1},
              {kModifiers + 4, 0xFFFFFFFFU},
              {kModifiers + 8, 2},
              {kModifiers + 8, 0xFFFFFFFFU},
              {kShots, 0},
              {kShots, 2},
              {kShots, 3},
              {kShots, 4},
              {kShots, 6},
              {kShots, 261},
              {kShots, 0xFFFFFFFFU}}}) {
        auto corrupt = bytes;
        writeWord(corrupt, offset, value);
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    player.rollback.reset();
    CHECK_FALSE(CombatPacket::encode(source));
    player.rollback = PlayerAnimator::Checkpoint{};
    player.modifiers.reset();
    CHECK_FALSE(CombatPacket::encode(source)); // fan cannot outlive its certificate
    player.weapon->shots = 1;
    const auto absent = encode(source);
    for (usize offset = 0; offset < CombatPacket::kModifierBytes; ++offset) {
        auto corrupt = absent;
        corrupt[kModifiers + offset] = 1;
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    for (const usize offset : {12U, 16U}) {
        auto corrupt = absent;
        writeWord(corrupt, kModifiers + offset, 0x80000000U);
        CHECK_FALSE(CombatPacket::decode(corrupt)); // absent clocks must be zero bytes
    }
    auto hidden = bytes;
    writeWord(hidden, kPlayer + 4, 8U | 64U | 256U);
    CHECK_FALSE(CombatPacket::decode(hidden));
    auto oldVersion = bytes;
    oldVersion[4] = 31;
    CHECK_FALSE(CombatPacket::decode(oldVersion));
}

TEST_CASE("Speed certificates preserve next-step pace and stale animation flags per seat",
          "[netplay][combat-snapshot][item-rollback][speed-rollback]") {
    for (const u32 mask : {0U, powerup::kRapidFire, PlayerMissiles::kPlainModifiers}) {
        for (const u32 ticks : {0U, 1U, PlayerWeaponModifiers::kMaxTicks}) {
            auto source = state(40, 0);
            for (usize seat = 0; seat < InputCommand::kSeats; ++seat) {
                auto& player = *source.players[seat];
                player.rollback = PlayerAnimator::Checkpoint{};
                // Pickup/expiry occurs after animation on the host. Its cached
                // flag and the next step's current inventory need not agree.
                player.rollback->speed = (seat & 1U) == 0;
                player.movement.speed = seat < 2 ? 8.0f : PlayerActor::kMaxSpeed;
                if (seat != 3) {
                    player.modifiers = PlayerWeaponModifiers{mask, ticks, true};
                }
            }
            const auto bytes = encode(source);
            const auto decoded = CombatPacket::decode(bytes);
            REQUIRE(decoded);
            CHECK(encode(*decoded) == bytes);
            for (usize seat = 0; seat < InputCommand::kSeats; ++seat) {
                const auto& player = *decoded->players[seat];
                REQUIRE(player.rollback);
                CHECK(player.rollback->speed == source.players[seat]->rollback->speed);
                CHECK(player.movement.speed == source.players[seat]->movement.speed);
                if (seat == 3) {
                    CHECK_FALSE(player.modifiers);
                } else {
                    REQUIRE(player.modifiers);
                    CHECK(player.modifiers->speed);
                    CHECK(player.modifiers->flags == mask);
                    CHECK(player.modifiers->ticks == ticks);
                }
            }
            auto& speedOnly = *source.players[0];
            speedOnly.modifiers = PlayerWeaponModifiers{0, ticks, true};
            REQUIRE(CombatPacket::encode(source));
            speedOnly.modifiers->speed = false;
            CHECK_FALSE(CombatPacket::encode(source));
            speedOnly.modifiers->speed = true;
            speedOnly.rollback.reset();
            CHECK_FALSE(CombatPacket::encode(source));
        }
    }
}

TEST_CASE("shield certificates and arm equipment round trip independently with fading clocks",
          "[netplay][combat-snapshot][item-rollback][shield-prediction]") {
    constexpr std::array shields{powerup::kReflectShield, powerup::kFireShield,
                                 powerup::kLightningShield};
    for (u32 mask = 1; mask < 8; ++mask) {
        u32 armor = 0;
        for (usize index = 0; index < shields.size(); ++index) {
            if ((mask & (1U << index)) != 0) {
                armor |= shields[index];
            }
        }
        for (const u32 ticks : {0U, 1U, PlayerWeaponModifiers::kMaxTicks}) {
            for (const f32 fire : {-1.0f, 0.01f, 0.8f, 10.0f}) {
                auto source = state(40, 0);
                for (usize seat = 0; seat < source.players.size(); ++seat) {
                    auto& player = source.players[seat].value();
                    player.rollback = PlayerAnimator::Checkpoint{};
                    player.rollback->shielded = (seat & 1U) != 0;
                    player.modifiers = PlayerWeaponModifiers{seat == 0 ? 0U : damage::kFire, ticks,
                                                             seat >= 2, armor};
                    if ((armor & powerup::kFireShield) != 0) {
                        player.modifiers->fireSeconds = fire;
                        player.modifiers->inventoryStep =
                            seat == 3 ? PlayerWeaponModifiers::kMaxInventoryStep
                                      : static_cast<f32>(seat) / 30;
                    }
                    player.armEquipment = armor | (seat == 3 ? powerup::kLeftGauntlet : 0U);
                }
                const auto bytes = encode(source);
                const auto decoded = CombatPacket::decode(bytes);
                REQUIRE(decoded);
                CHECK(encode(*decoded) == bytes);
                for (usize seat = 0; seat < source.players.size(); ++seat) {
                    const auto& expected = source.players[seat].value();
                    const auto& actual = decoded->players[seat].value();
                    REQUIRE(actual.modifiers);
                    CHECK(actual.modifiers->shields == armor);
                    CHECK(actual.modifiers->flags == expected.modifiers->flags);
                    CHECK(actual.modifiers->ticks == ticks);
                    CHECK(actual.modifiers->speed == expected.modifiers->speed);
                    CHECK(actual.modifiers->fireSeconds == expected.modifiers->fireSeconds);
                    CHECK(actual.modifiers->inventoryStep == expected.modifiers->inventoryStep);
                    CHECK(actual.rollback->shielded == expected.rollback->shielded);
                    CHECK(actual.armEquipment == expected.armEquipment);
                }
            }
        }
    }
    // Rendering an authoritative attachment does not require action replay.
    auto source = state(40, 0);
    for (const u32 armor : {0U, powerup::kLeftGauntlet, powerup::kShields,
                            powerup::kLeftGauntlet | powerup::kShields}) {
        source.players[0]->armEquipment = armor;
        const auto decoded = CombatPacket::decode(encode(source));
        REQUIRE(decoded);
        CHECK(decoded->players[0]->armEquipment == armor);
        CHECK_FALSE(decoded->players[0]->modifiers);
        CHECK_FALSE(decoded->players[0]->rollback);
    }
}

TEST_CASE("shield wire rejects unknown equipment hidden clocks and malformed fade values",
          "[netplay][combat-snapshot][item-rollback][shield-prediction]") {
    auto source = state(40, 0);
    auto& player = source.players[0].value();
    player.rollback = PlayerAnimator::Checkpoint{};
    player.modifiers = PlayerWeaponModifiers{0, 120, false, powerup::kFireShield, 0.5f, 1.0f / 60};
    player.armEquipment = powerup::kFireShield;
    const auto bytes = encode(source);
    constexpr usize kPlayer = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes;
    constexpr usize kEquipment =
        kPlayer + CombatPacket::kPlayerBytes - CombatPacket::kArmEquipmentBytes;
    constexpr usize kModifiers = kEquipment - CombatPacket::kModifierBytes;
    for (const u32 unknown : {2U, powerup::kLeftGauntlet, powerup::kInvulnerable,
                              powerup::kGoldInvulnerable, 0x80000000U, 0xFFFFFFFFU}) {
        auto corrupt = bytes;
        writeWord(corrupt, kModifiers + 8, powerup::kFireShield | unknown);
        CHECK_FALSE(CombatPacket::decode(corrupt));
        player.modifiers->shields = powerup::kFireShield | unknown;
        CHECK_FALSE(CombatPacket::encode(source));
    }
    player.modifiers->shields = powerup::kFireShield;
    for (const u32 unknown :
         {1U, powerup::kInvulnerable, powerup::kGoldInvulnerable, 0x80000000U, 0xFFFFFFFFU}) {
        auto corrupt = bytes;
        writeWord(corrupt, kEquipment, powerup::kFireShield | unknown);
        CHECK_FALSE(CombatPacket::decode(corrupt));
        player.armEquipment = powerup::kFireShield | unknown;
        CHECK_FALSE(CombatPacket::encode(source));
    }
    player.armEquipment = powerup::kFireShield;
    for (const u32 bits : {0U, std::bit_cast<u32>(-0.5f), std::bit_cast<u32>(-2.0f), 0x7FC00000U,
                           0x7F800000U, 0xFF800000U}) {
        auto corrupt = bytes;
        writeWord(corrupt, kModifiers + 12, bits);
        CHECK_FALSE(CombatPacket::decode(corrupt));
        player.modifiers->fireSeconds = std::bit_cast<f32>(bits);
        CHECK_FALSE(CombatPacket::encode(source));
    }
    player.modifiers->fireSeconds = 0.5f;
    for (const u32 bits : {std::bit_cast<u32>(-0.001f), std::bit_cast<u32>(0.101f), 0x7FC00000U,
                           0x7F800000U, 0xFF800000U}) {
        auto corrupt = bytes;
        writeWord(corrupt, kModifiers + 16, bits);
        CHECK_FALSE(CombatPacket::decode(corrupt));
        player.modifiers->inventoryStep = std::bit_cast<f32>(bits);
        CHECK_FALSE(CombatPacket::encode(source));
    }
    player.modifiers->inventoryStep = 0;
    player.modifiers->shields = powerup::kReflectShield;
    CHECK_FALSE(CombatPacket::encode(source)); // no Fire Shield means no fade state
    player.modifiers->fireSeconds = 0;
    CHECK(CombatPacket::encode(source));
    player.modifiers->inventoryStep = 1.0f / 60;
    CHECK_FALSE(CombatPacket::encode(source));
    player.modifiers->inventoryStep = 0;
    player.rollback.reset();
    CHECK_FALSE(CombatPacket::encode(source));
    player.modifiers.reset();
    const auto absent = encode(source);
    for (usize offset = 0; offset < CombatPacket::kModifierBytes; ++offset) {
        auto corrupt = absent;
        corrupt[kModifiers + offset] = 1;
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    for (usize cut = 1; cut <= CombatPacket::kArmEquipmentBytes; ++cut) {
        CHECK_FALSE(CombatPacket::decode(std::span(bytes).first(bytes.size() - cut)));
    }
    auto old = bytes;
    old[4] = 31;
    CHECK_FALSE(CombatPacket::decode(old));
}

TEST_CASE("elemental weapon descriptors preserve separate rider clocks and modifier certificates",
          "[netplay][combat-snapshot][item-rollback][elemental-rollback]") {
    for (const u32 element : {damage::kFire, damage::kElectric, damage::kLight, damage::kAcid}) {
        for (const u32 fan :
             {0U, powerup::kThreeWayShot, powerup::kFiveWayShot, PlayerMissiles::kPlainModifiers}) {
            for (const bool clock : {false, true}) {
                auto source = state(40, 0);
                for (usize seat = 0; seat < InputCommand::kSeats; ++seat) {
                    auto& player = source.players[seat].value();
                    player.rollback = PlayerAnimator::Checkpoint{};
                    player.modifiers = PlayerWeaponModifiers{element | fan, seat == 0 ? 0U : 120U,
                                                             (seat & 1U) != 0};
                    PowerupEffects effects;
                    effects.weapon = element | fan;
                    PlayerWeaponState weapon;
                    // Include ordinary weapons and the two effect-only caster classes.
                    weapon.character = static_cast<u32>(seat * 2);
                    weapon.resource = static_cast<u32>(seat + 1);
                    weapon.rider = seat == 0 ? 65535U : static_cast<u32>(seat + 10);
                    weapon.shots = static_cast<u8>(effects.shots());
                    weapon.textureFrame = 19.25f;
                    if (clock) {
                        weapon.riderTextureFrame = seat == 0 ? 0.0f : 1'000'000'000.0f;
                    }
                    player.weapon = weapon;
                }
                const auto bytes = encode(source);
                const auto decoded = CombatPacket::decode(bytes);
                REQUIRE(decoded);
                CHECK(encode(*decoded) == bytes);
                for (usize seat = 0; seat < InputCommand::kSeats; ++seat) {
                    const auto& expected = source.players[seat].value();
                    const auto& actual = decoded->players[seat].value();
                    REQUIRE(actual.weapon);
                    REQUIRE(actual.modifiers);
                    CHECK(actual.weapon->resource == expected.weapon->resource);
                    CHECK(actual.weapon->rider == expected.weapon->rider);
                    CHECK(actual.weapon->textureFrame == expected.weapon->textureFrame);
                    CHECK(actual.weapon->riderTextureFrame == expected.weapon->riderTextureFrame);
                    CHECK(actual.weapon->character == expected.weapon->character);
                    CHECK(actual.weapon->shots == expected.weapon->shots);
                    CHECK(actual.modifiers->flags == expected.modifiers->flags);
                    CHECK(actual.modifiers->ticks == expected.modifiers->ticks);
                    CHECK(actual.modifiers->speed == expected.modifiers->speed);
                }
            }
        }
    }
}

TEST_CASE("elemental wire rejects missing resources invalid elements and hidden rider clocks",
          "[netplay][combat-snapshot][item-rollback][elemental-rollback]") {
    auto source = state(40, 0);
    auto& player = source.players[0].value();
    player.rollback = PlayerAnimator::Checkpoint{};
    player.modifiers = PlayerWeaponModifiers{damage::kFire | powerup::kRapidFire, 120};
    player.weapon = PlayerWeaponState{};
    player.weapon->resource = 1;
    player.weapon->rider = 7;
    player.weapon->textureFrame = 19.25f;
    player.weapon->riderTextureFrame = 72.5f;
    const auto bytes = encode(source);
    constexpr usize kPlayer = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes;
    constexpr usize kModifiers = kPlayer + CombatPacket::kPlayerBytes -
                                 CombatPacket::kModifierBytes - CombatPacket::kArmEquipmentBytes;
    constexpr usize kWeapon =
        kModifiers - CombatPacket::kKnockbackBytes - CombatPacket::kWeaponBytes;
    for (const auto [offset, value] : std::array<std::pair<usize, u32>, 9>{
             {{kWeapon + 20, 0},
              {kWeapon + 60, 0},
              {kWeapon + 60, 65536},
              {kWeapon + 60, 0xFFFFFFFFU},
              {kWeapon + 36, 2},
              {kWeapon + 36, 8},
              {kModifiers, 0},
              {kModifiers, powerup::kRapidFire},
              {kModifiers, powerup::kThunderHammer | damage::kFire}}}) {
        auto corrupt = bytes;
        writeWord(corrupt, offset, value);
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    for (const u32 element : {5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U}) {
        auto corrupt = bytes;
        writeWord(corrupt, kModifiers, powerup::kRapidFire | element);
        CHECK_FALSE(CombatPacket::decode(corrupt));
        player.modifiers->flags = powerup::kRapidFire | element;
        CHECK_FALSE(CombatPacket::encode(source));
    }
    player.modifiers->flags = damage::kFire | powerup::kRapidFire;
    for (const u32 bits : {0x7FC00000U, 0x7F800000U, 0xFF800000U, std::bit_cast<u32>(-1.0f),
                           std::bit_cast<u32>(1'000'001'024.0f)}) {
        auto corrupt = bytes;
        writeWord(corrupt, kWeapon + 64, bits);
        CHECK_FALSE(CombatPacket::decode(corrupt));
        player.weapon->riderTextureFrame = std::bit_cast<f32>(bits);
        CHECK_FALSE(CombatPacket::encode(source));
    }
    player.weapon->riderTextureFrame.reset();
    const auto noClock = encode(source);
    for (const u32 hidden : {1U, 0x80000000U, 0x3F800000U, 0x7FC00000U}) {
        auto corrupt = noClock;
        writeWord(corrupt, kWeapon + 64, hidden);
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    player.weapon->rider = 0;
    CHECK_FALSE(CombatPacket::encode(source)); // elemental previews cannot guess missing assets
    player.weapon.reset();
    CHECK(CombatPacket::encode(source)); // the actor's modifier envelope remains useful
    player.weapon = PlayerWeaponState{};
    player.weapon->resource = 1;
    player.weapon->rider = 7;
    player.modifiers->flags = powerup::kRapidFire;
    CHECK_FALSE(CombatPacket::encode(source));
    player.modifiers.reset();
    CHECK_FALSE(CombatPacket::encode(source));
    player.weapon->rider = 0;
    CHECK(CombatPacket::encode(source));
    player.weapon->riderTextureFrame = 0.0f;
    CHECK_FALSE(CombatPacket::encode(source)); // even frame zero needs an actual rider
}

TEST_CASE("body rider and streak share a release without sharing their resource identity",
          "[netplay][combat-snapshot][rollback][elemental-rollback]") {
    auto source = rollbackShotState();
    const auto body = source.projectiles.front();
    for (const auto kind : {ProjectileSource::PlayerEffect, ProjectileSource::Streak,
                            ProjectileSource::PlayerRider}) {
        auto component = body;
        component.source = kind;
        component.resource = body.resource + static_cast<u32>(kind);
        component.textureFrame = static_cast<f32>(kind) + 0.25f;
        source.projectiles.push_back(component);
    }
    const auto bytes = encode(source);
    const auto decoded = CombatPacket::decode(bytes);
    REQUIRE(decoded);
    REQUIRE(decoded->projectiles.size() == 4);
    CHECK(encode(*decoded) == bytes);
    for (const auto& component : decoded->projectiles) {
        REQUIRE(component.rollback);
        CHECK(component.rollback->origin == body.rollback->origin);
        CHECK(component.rollback->flight == body.rollback->flight);
    }
    // A rider is still a separately ordered visual component, not a second release.
    source.projectiles.push_back(source.projectiles.back());
    CHECK_FALSE(CombatPacket::encode(source));
    source.projectiles.pop_back();
    auto& rider = source.projectiles.back();
    ++rider.rollback->origin.epoch;
    CHECK_FALSE(CombatPacket::encode(source));
    rider.rollback->origin = body.rollback->origin;
    ++rider.rollback->origin.grant;
    CHECK_FALSE(CombatPacket::encode(source));
    rider.rollback->origin = body.rollback->origin;
    rider.rollback->origin.seat = 4;
    CHECK_FALSE(CombatPacket::encode(source));
    constexpr usize kFirstShot = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                                 InputCommand::kSeats * CombatPacket::kPlayerBytes;
    auto unknown = bytes;
    writeWord(unknown, kFirstShot + 3 * CombatPacket::kProjectileBytes,
              static_cast<u32>(ProjectileSource::PlayerRider) + 1);
    CHECK_FALSE(CombatPacket::decode(unknown));
}

TEST_CASE("projectile fan ordinals are independently encoded and reject unknown values",
          "[netplay][combat-snapshot][item-rollback]") {
    auto source = rollbackShotState();
    for (u8 ordinal = 0; ordinal < 5; ++ordinal) {
        source.projectiles.front().rollback->origin.ordinal = ordinal;
        const auto bytes = encode(source);
        const auto decoded = CombatPacket::decode(bytes);
        REQUIRE(decoded);
        CHECK(decoded->projectiles.front().rollback->origin.ordinal == ordinal);
        CHECK(encode(*decoded) == bytes);
    }
    constexpr usize kIdentity = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                                InputCommand::kSeats * CombatPacket::kPlayerBytes +
                                CombatPacket::kProjectileBytes - CombatPacket::kShotRollbackBytes -
                                CombatPacket::kParticleBytes - CombatPacket::kEffectOriginBytes -
                                CombatPacket::kPotionRollbackBytes + 32;
    const auto bytes = encode(source);
    for (const u32 value : {5U << 16U, 6U << 16U, 7U << 16U, 8U << 16U, 1U << 19U}) {
        auto corrupt = bytes;
        writeWord(corrupt, kIdentity, value);
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    source.projectiles.front().rollback->origin.ordinal = 5;
    CHECK_FALSE(CombatPacket::encode(source));
}

TEST_CASE("combat snapshots preserve bounded death appearance and the host texture clock",
          "[netplay][combat-snapshot]") {
    auto source = state(40, 1);
    source.worldTextureFrame = 123.5f;
    auto& enemy = source.enemies.front();
    enemy.life = ReplicaEnemyLife::Dying;
    enemy.alpha = 0.25f;
    enemy.scale = 0.5f;
    enemy.deathSkin = 6;
    enemy.deathFrame = 14;
    auto received = CombatPacket::decode(encode(source));
    REQUIRE(received);
    CHECK(received->worldTextureFrame == source.worldTextureFrame);
    CHECK(received->enemies.front().alpha == enemy.alpha);
    CHECK(received->enemies.front().scale == enemy.scale);
    CHECK(received->enemies.front().deathSkin == 6);
    CHECK(received->enemies.front().deathFrame == 14);
    for (const u32 invalid : {7U, 65535U}) {
        enemy.deathSkin = invalid;
        CHECK_FALSE(CombatPacket::encode(source));
    }
    enemy.deathSkin = 6;
    enemy.deathFrame = 15;
    CHECK_FALSE(CombatPacket::encode(source));
    enemy.deathFrame = 0;
    enemy.life = ReplicaEnemyLife::Active;
    CHECK_FALSE(CombatPacket::encode(source));
    enemy.deathSkin = 0;
    source.worldTextureFrame = -1;
    CHECK_FALSE(CombatPacket::encode(source));
}

TEST_CASE("player shadow contacts round trip and reject invalid floor normals and hidden owners",
          "[netplay][combat-snapshot][replica-shadows]") {
    auto source = state(0, 0);
    source.players[0]->shadow = PlayerShadowState{{3, -5, 9}, {0.6f, 0.8f, 0}, 0.75f};
    const auto bytes = encode(source);
    const auto decoded = CombatPacket::decode(bytes);
    REQUIRE(decoded);
    REQUIRE(decoded->players[0]->shadow);
    CHECK(decoded->players[0]->shadow->ground == Vec3{3, -5, 9});
    CHECK(decoded->players[0]->shadow->normal == Vec3{0.6f, 0.8f, 0});
    CHECK(decoded->players[0]->shadow->alpha == 0.75f);
    CHECK(encode(*decoded) == bytes);
    constexpr usize kShadow = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                              CombatPacket::kPlayerBytes - CombatPacket::kShadowBytes -
                              CombatPacket::kMovementBytes - CombatPacket::kRollbackBytes -
                              CombatPacket::kRollbackInputBytes - CombatPacket::kWeaponBytes -
                              CombatPacket::kKnockbackBytes - CombatPacket::kModifierBytes -
                              CombatPacket::kArmEquipmentBytes;
    for (usize field = 0; field < 7; ++field) {
        auto corrupt = bytes;
        writeWord(corrupt, kShadow + field * 4, 0x7FC00000);
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    for (const Vec3 normal : {Vec3{0}, Vec3{0, -1, 0}, Vec3{2, 1, 0}, Vec3{1, 0, 0}}) {
        auto bad = source;
        bad.players[0]->shadow->normal = normal;
        CHECK_FALSE(CombatPacket::encode(bad));
    }
    auto bad = source;
    bad.players[0]->life = ReplicaPlayerLife::InTower;
    CHECK_FALSE(CombatPacket::encode(bad));
    bad = source;
    bad.players[0]->portalPhase = 1.0f;
    CHECK_FALSE(CombatPacket::encode(bad));
    bad = source;
    bad.players[0]->shadow->alpha = 1.1f;
    CHECK_FALSE(CombatPacket::encode(bad));
    auto absent = encode(state(0, 0));
    writeWord(absent, kShadow, 1);
    CHECK_FALSE(CombatPacket::decode(absent));
}

TEST_CASE("companion checkpoints preserve both slots and reject malformed or ownerless meshes",
          "[netplay][combat-snapshot][replica-companions]") {
    auto source = state(5, 0);
    for (u32 form = 1; form <= 7; ++form) {
        for (auto& player : source.players) {
            CompanionState earned;
            earned.form = 2;
            earned.placement[3] = {1, 2, 3, 1};
            earned.animation = {0, 1, 0x123456789ULL, 7.5f, 1};
            earned.textureClock = 35.5f;
            earned.alpha = 0.4f;
            player->companions[0] = earned;
            earned.form = form;
            player->companions[1] = earned;
        }
        const auto bytes = encode(source);
        const auto decoded = CombatPacket::decode(bytes);
        REQUIRE(decoded);
        CHECK(encode(*decoded) == bytes);
    }
    constexpr usize kFirst = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes + 36;
    const auto valid = encode(source);
    for (const auto [offset, value] :
         std::array<std::pair<usize, u32>, 10>{{{kFirst, 3},
                                                {kFirst + CombatPacket::kCompanionBytes, 8},
                                                {kFirst + 4, 0x7FC00000},
                                                {kFirst + 40, 0x7F800000},
                                                {kFirst + 52, 1},
                                                {kFirst + 56, 65536},
                                                {kFirst + 68, 0xBF800000},
                                                {kFirst + 72, 0},
                                                {kFirst + 76, 0x7FC00000},
                                                {kFirst + 80, 0x40000000}}}) {
        auto bytes = valid;
        writeWord(bytes, offset, value);
        CHECK_FALSE(CombatPacket::decode(bytes));
    }
    auto invalid = source;
    invalid.players[0]->life = ReplicaPlayerLife::InTower;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    invalid.players[0]->companions[0]->animation.generation = 0;
    CHECK_FALSE(CombatPacket::encode(invalid));
    auto absent = encode(state(5, 0));
    // An absent slot is canonical zero padding, not a second hidden payload.
    writeWord(absent, kFirst + 4, 1);
    CHECK_FALSE(CombatPacket::decode(absent));
}

TEST_CASE("fire companion seeds retain native clock precision and reject hidden or foreign state",
          "[netplay][combat-snapshot][replica-companions][shield-prediction]") {
    auto source = state(5, 0);
    CompanionState fire;
    fire.form = 2;
    fire.animation = {0, 0, 0x123456789ULL, 7.5f, 1};
    fire.textureClock = 35.5f;
    fire.alpha = 0.4f;
    fire.rollback = AnimationPlayer::Checkpoint{};
    fire.rollback->generation = fire.animation.generation;
    fire.rollback->playing = true;
    fire.rollback->time = 0.12345678901234;
    fire.rollback->frame = 7;
    fire.rollback->speed = 1.5f;
    fire.rollback->smooth = true;
    for (auto& player : source.players) {
        player->companions[1] = fire;
    }
    const auto bytes = encode(source);
    const auto decoded = CombatPacket::decode(bytes);
    REQUIRE(decoded);
    CHECK(encode(*decoded) == bytes);
    for (const auto& player : decoded->players) {
        REQUIRE(player->companions[1]);
        CHECK(player->companions[1]->rollback == fire.rollback);
    }
    constexpr usize kSecond =
        CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes + 36 + CombatPacket::kCompanionBytes;
    constexpr usize kPresent = kSecond + 84;
    constexpr usize kSeed = kPresent + 4;
    for (const auto [offset, value] :
         std::array<std::pair<usize, u32>, 12>{{{kSecond, 1},
                                                {kSecond, 3},
                                                {kSecond + 56, 1},
                                                {kPresent, 0},
                                                {kPresent, 2},
                                                {kPresent, 0xFFFFFFFFU},
                                                {kSeed, 1},
                                                {kSeed + 4, 0},
                                                {kSeed + 12, 0x7FC00000U},
                                                {kSeed + 24, 0x7F800000U},
                                                {kSeed + 40, 0},
                                                {kSeed + 40, 16}}}) {
        auto corrupt = bytes;
        writeWord(corrupt, offset, value);
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    auto& companion = source.players[0]->companions[1].value();
    companion.rollback->playing = false;
    CHECK_FALSE(CombatPacket::encode(source));
    companion = fire;
    source.players[0]->companions[0] = fire;
    CHECK_FALSE(CombatPacket::encode(source)); // earned familiar form2 is not Fire Shield
    source.players[0]->companions[0].reset();
    companion.rollback.reset();
    const auto noSeed = encode(source);
    for (usize offset = 0; offset < AnimationCheckpoint::kBytes; ++offset) {
        auto corrupt = noSeed;
        corrupt[kSeed + offset] = 1;
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    source.players[0]->companions[1].reset();
    const auto absent = encode(source);
    for (usize offset = 84; offset < CombatPacket::kCompanionBytes; ++offset) {
        auto corrupt = absent;
        corrupt[kSecond + offset] = 1;
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
}

TEST_CASE("portal phase round trips independently of collision positions and rejects bad wire data",
          "[netplay][combat-snapshot][online-departure]") {
    auto source = state(5, 0);
    for (usize seat = 0; seat < source.players.size(); ++seat) {
        source.players[seat]->portalPhase = static_cast<f32>(seat) / 3;
    }
    const auto bytes = encode(source);
    const auto decoded = CombatPacket::decode(bytes);
    REQUIRE(decoded);
    CHECK(encode(*decoded) == bytes);
    for (usize seat = 0; seat < source.players.size(); ++seat) {
        CHECK(decoded->players[seat]->portalPhase == source.players[seat]->portalPhase);
        CHECK(decoded->motion.players[seat]->position == source.motion.players[seat]->position);
    }
    constexpr usize kPlayer = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes;
    for (const u32 value : {0x7FC00000U, 0x7F800000U, 0xBF800000U, 0x3F800001U}) {
        auto invalid = bytes;
        writeWord(invalid, kPlayer + 32, value);
        CHECK_FALSE(CombatPacket::decode(invalid));
    }
    for (const auto life : {ReplicaPlayerLife::Dying, ReplicaPlayerLife::InTower}) {
        auto invalid = source;
        invalid.players[0]->life = life;
        CHECK_FALSE(CombatPacket::encode(invalid));
    }
    auto absent = encode(state(5, 0));
    writeWord(absent, kPlayer + 32, 0x3F000000U);
    CHECK_FALSE(CombatPacket::decode(absent)); // no hidden phase behind a cleared presence flag
    auto oldVersion = bytes;
    oldVersion[4] = 16;
    CHECK_FALSE(CombatPacket::decode(oldVersion));
}

TEST_CASE("pickup records preserve affine placements and reject malformed rosters",
          "[netplay][combat-snapshot]") {
    auto source = state(3, 0);
    source.pickups.push_back(PickupState{.instance = 100,
                                         .resource = 4,
                                         .animation = {0, 3, 10, 2.5f, 1},
                                         .textureFrame = 43.5f,
                                         .alpha = 0.5f});
    source.pickups[0].placement = glm::scale(
        glm::rotate(glm::translate(Mat4{1}, Vec3{4, 5, 6}), 0.7f, Vec3{0, 1, 0}), Vec3{1, 2, 3});
    const auto bytes = encode(source);
    const auto decoded = CombatPacket::decode(bytes);
    REQUIRE(decoded);
    CHECK(encode(*decoded) == bytes);
    constexpr usize kPickup =
        CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes + 4 * CombatPacket::kPlayerBytes;
    CHECK(bytes.size() == kPickup + CombatPacket::kPickupBytes);
    for (usize length = kPickup; length < bytes.size(); ++length) {
        CHECK_FALSE(CombatPacket::decode(std::span(bytes).first(length)));
    }
    for (const usize offset : {usize{20}, kPickup + 8, kPickup + 64}) {
        auto bad = bytes;
        writeWord(bad, offset, 0xFFFFFFFF);
        CHECK_FALSE(CombatPacket::decode(bad));
    }
    for (const usize offset : {kPickup, kPickup + 8, kPickup + 12}) {
        auto bad = bytes;
        writeWord(bad, offset, 0);
        CHECK_FALSE(CombatPacket::decode(bad));
    }
    for (usize offset = kPickup + 16; offset < kPickup + 64; offset += 4) {
        auto bad = bytes;
        writeWord(bad, offset, 0x7FC00000);
        CHECK_FALSE(CombatPacket::decode(bad));
    }
    for (const usize offset : {kPickup + 80, kPickup + 84, kPickup + 88, kPickup + 92}) {
        auto bad = bytes;
        writeWord(bad, offset, 0x7FC00000);
        CHECK_FALSE(CombatPacket::decode(bad));
    }
    auto bad = source;
    bad.pickups[0].placement[0].w = 1;
    CHECK_FALSE(CombatPacket::encode(bad));
    bad = source;
    bad.pickups.push_back(bad.pickups[0]);
    CHECK_FALSE(CombatPacket::encode(bad));
    bad.pickups.back().instance = 99;
    CHECK_FALSE(CombatPacket::encode(bad));
}

TEST_CASE("fixture codecs preserve independent cursors and reject malformed visible rosters",
          "[netplay][combat-snapshot]") {
    auto source = state(3, 0);
    for (u32 kind = 0; kind <= static_cast<u32>(FixtureSource::SecretPortal); ++kind) {
        FixtureState fixture;
        fixture.source = static_cast<FixtureSource>(kind);
        fixture.instance = 0x123456789ABCDEF0ULL;
        fixture.resource = kind + 1;
        fixture.pose = {0, 1, 7, 9.5f, 1};
        fixture.meshSequence = 2;
        fixture.textureSequence = 3;
        fixture.textureFrame = 4;
        fixture.textureClock = 66.5f;
        fixture.cameraFacing = true;
        source.fixtures.push_back(fixture);
    }
    const auto bytes = encode(source);
    const auto decoded = CombatPacket::decode(bytes);
    REQUIRE(decoded);
    CHECK(encode(*decoded) == bytes);
    constexpr usize kFixture =
        CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes + 4 * CombatPacket::kPlayerBytes;
    CHECK(bytes.size() == kFixture + 12 * CombatPacket::kFixtureBytes);
    for (usize length = kFixture; length < bytes.size(); ++length) {
        CHECK_FALSE(CombatPacket::decode(std::span(bytes).first(length)));
    }
    for (const auto [offset, value] : std::array<std::pair<usize, u32>, 9>{
             {{24, static_cast<u32>(CombatSnapshot::kMaxFixtures + 1)},
              {kFixture, 12},
              {kFixture + 12, 0},
              {kFixture + 16, 0},
              {kFixture + 68, 1},
              {kFixture + 92, 65536},
              {kFixture + 100, 65536},
              {kFixture + 112, 0x40000000},
              {kFixture + 116, 2}}}) {
        auto invalid = bytes;
        writeWord(invalid, offset, value);
        CHECK_FALSE(CombatPacket::decode(invalid));
    }
    for (usize offset = 20; offset < 68; offset += 4) {
        auto invalid = bytes;
        writeWord(invalid, kFixture + offset, 0x7FC00000);
        CHECK_FALSE(CombatPacket::decode(invalid));
    }
    for (const usize offset : {84U, 88U, 96U, 104U, 108U, 112U}) {
        auto invalid = bytes;
        writeWord(invalid, kFixture + offset, 0x7FC00000);
        CHECK_FALSE(CombatPacket::decode(invalid));
    }
    auto invalid = source;
    invalid.fixtures[1] = invalid.fixtures[0];
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    std::swap(invalid.fixtures[0], invalid.fixtures[1]);
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    invalid.fixtures[0].instance = 0;
    CHECK_FALSE(CombatPacket::encode(invalid));
}

TEST_CASE("combat chunks commit atomically despite reordering and duplicates",
          "[netplay][combat-snapshot]") {
    CombatReplica replica;
    REQUIRE(replica.begin(1, 9));
    commit(replica, state(1, 0));
    auto next = state(2, CombatSnapshot::kMaxEnemies);
    next.geometry = SceneGeometry{1, static_cast<u32>(WorldMotion::kMaxObjects), 0, {}};
    for (usize i = 0; i < SceneGeometry::kMaxStates; ++i) {
        next.geometry->objects.push_back({static_cast<u32>(i)});
    }
    auto& world = next.worldMotion.emplace();
    world.collision.resize(WorldMotion::kMaxObjects, WorldMotion::kSolid);
    for (usize i = 0; i < WorldMotion::kMaxAnimations; ++i) {
        world.animations.push_back({static_cast<s32>(i), 2, 0.5f});
    }
    for (usize i = 0; i < LevelTriggers::Motion::kMaxTargets; ++i) {
        world.triggers.targets.push_back({static_cast<s32>(i)});
    }
    for (usize i = 0; i < Rotators::kMaxCursors; ++i) {
        world.rotators.push_back({static_cast<s32>(i), static_cast<s32>(i)});
    }
    REQUIRE(world.valid());
    for (usize i = 0; i < CombatSnapshot::kMaxProjectiles; ++i) {
        ProjectileState shot;
        shot.source = ProjectileSource::PlayerRider;
        shot.instance = i + 1;
        shot.resource = 1;
        auto& particles = shot.particles.emplace();
        particles.character = static_cast<u32>(i % 17);
        particles.origin = ShotIdentity{next.motion.epoch, 1, i + 1, 1, 0, false};
        particles.flight.birth.position = Vec3{static_cast<f32>(i), 2, -3};
        particles.flight.birth.velocity = {17, 0, 4};
        particles.flight.activeSteps = 1;
        particles.flight.retiring = (i % 2) != 0;
        particles.flight.totalSteps = particles.flight.retiring ? 10 : 1;
        next.projectiles.push_back(shot);
    }
    for (usize i = 0; i < CombatSnapshot::kMaxPickups; ++i) {
        PickupState item;
        item.instance = i + 1;
        item.resource = 1;
        next.pickups.push_back(item);
    }
    for (usize i = 0; i < CombatSnapshot::kMaxFixtures; ++i) {
        FixtureState fixture;
        fixture.instance = i + 1;
        fixture.resource = 1;
        next.fixtures.push_back(fixture);
    }
    for (usize i = 0; i < FighterPacket::kMaxMeshes; ++i) {
        FighterMeshState mesh;
        mesh.incarnation = mesh.resource = 1;
        mesh.part = static_cast<u32>(i + 1);
        mesh.nodes.resize(FighterPacket::kMaxNodes / FighterPacket::kMaxMeshes);
        next.fighters.push_back(std::move(mesh));
    }
    next.hud = HudSnapshot{};
    for (auto& player : next.hud->players) {
        player = HudPlayer{};
    }
    next.hud->cards.resize(HudSnapshot::kMaxCards);
    for (auto& count : next.hud->counts) {
        count = HudCount{};
    }
    next.hud->bossBars.resize(HudSnapshot::kMaxBossBars);
    for (usize seat = 0; seat < next.players.size(); ++seat) {
        auto& player = next.players[seat].value();
        player.floor = PlayerFloorState{static_cast<s32>(seat), 0, std::nullopt};
        player.movement.enabled = true;
        player.movement.current = true;
        player.rollback = PlayerAnimator::Checkpoint{};
        player.knockback = knockbackState().players[0].value().knockback;
        player.reaction = PlayerDeed::None;
        player.knockbackTicks = 0;
        auto& melee = player.melee.emplace();
        melee.selection.feet = next.motion.players[seat].value().position;
        melee.selection.reach = 1.75f;
        melee.selection.targets.resize(PlayerMeleeState::kMaxTargets);
        melee.moving = movingMelee(melee.selection.feet, PlayerMeleeState::kMaxTargets,
                                   PlayerMeleeState::kMaxObstacles);
        player.turboGeometry = melee;
        auto& turbo = player.turbo.emplace();
        turbo.meter = {100, 100, TurboMeter::Flash::Glow, TurboMeter::kGlowTicks};
        turbo.move.owed = 100;
        turbo.move.pending.assign(TurboMove::kMaxCheckpointRows, 0);
        turbo.move.all = turbo.move.pending;
        turbo.move.volleysShot.assign(TurboMove::kMaxCheckpointRows, 0);
        auto& contacts = player.locomotion.emplace(locomotionContacts(melee.selection.feet));
        contacts.bodies.resize(PlayerLocomotionState::kMaxBodies, contacts.bodies.front());
        contacts.generators.resize(PlayerLocomotionState::kMaxGenerators,
                                   contacts.generators.front());
        contacts.fixtures.resize(PlayerLocomotionState::kMaxFixtures, contacts.fixtures.front());
        contacts.party.resize(PlayerLocomotionState::kMaxParty, contacts.party.front());
        contacts.automatic = true;
        contacts.automaticGeometry = melee;
        player.rollback->animation.transitionLength = 0.125f;
        player.rollback->animation.transitionTime = 0.03125f;
        player.transitionPose =
            blendPose(TreePose::Checkpoint::kMaxNodes, TreePose::Checkpoint::kMaxName);
        player.turboEffects = TurboEffectState{100, TextureAnimator::Checkpoint{27, 0.5f, 0.25f},
                                               TextureAnimator::Checkpoint{45, 0.25f, 0.5f}};
        auto& potion = player.potion.emplace();
        potion.potions.assign(Inventory::kMostPotions, 3);
        potion.bonuses[3] = damage::kOwnColourBonus;
        potion.powers[3] *= potion.bonuses[3];
        potion.damageFlags = damage::kHeal;
        potion.ticks = contacts.ticks;
        const auto contactBytes = PlayerLocomotionCheckpoint::encode(contacts);
        const auto poseBytes = PlayerPoseCheckpoint::encode(*player.transitionPose);
        const auto effectBytes = TurboEffectCheckpoint::encode(*player.turboEffects);
        const auto potionBytes = PlayerPotionCheckpoint::encode(potion);
        REQUIRE(contactBytes);
        REQUIRE(poseBytes);
        REQUIRE(effectBytes);
        REQUIRE(potionBytes);
        CHECK(contactBytes->size() == PlayerLocomotionCheckpoint::kMaxBytes);
        CHECK(poseBytes->size() == PlayerPoseCheckpoint::kMaxBytes);
        CHECK(effectBytes->size() == TurboEffectCheckpoint::kMaxBytes);
        CHECK(potionBytes->size() == PlayerPotionCheckpoint::kMaxBytes);
    }
    // The new typed contacts add 16 bytes per body/party/generator entry.
    // Combined component maxima now exceed the fixed schema-39 packet budget;
    // neither encoding nor compression is allowed to silently prune that state.
    CHECK(CombatPacket::kMaxBytes == 1'570'903);
    CHECK(CombatReplica::kMaxChunks == 1535);
    REQUIRE(next.valid());
    CHECK_FALSE(CombatPacket::encode(next));
    CHECK_FALSE(CombatReplica::packets(next, SnapshotBlock::Compression::Automatic));
    CHECK(next.fixtures.size() == CombatSnapshot::kMaxFixtures);
    CHECK(next.players[0]->locomotion->bodies.size() == PlayerLocomotionState::kMaxBodies);
    // Build a separately chosen exact-budget fixture for the fragmentation
    // test. This is not an encoder fallback: all 920 requested fixtures remain
    // intact through encoding, reordered delivery and loss recovery.
    constexpr usize kTypedGrowth =
        InputCommand::kSeats * 16 *
        (PlayerLocomotionState::kMaxBodies + PlayerLocomotionState::kMaxParty +
         PlayerLocomotionState::kMaxGenerators);
    static_assert(kTypedGrowth % CombatPacket::kFixtureBytes == 0);
    next.fixtures.resize(CombatSnapshot::kMaxFixtures - kTypedGrowth / CombatPacket::kFixtureBytes);
    CHECK(encode(next).size() == CombatPacket::kMaxBytes);
    // The optional complete island still competes for that same fixed budget.
    next.opponents = opponentState();
    next.opponentFrame = opponentFrame(next);
    REQUIRE(next.valid());
    CHECK_FALSE(CombatPacket::encode(next));
    CHECK_FALSE(CombatReplica::packets(next, SnapshotBlock::Compression::Automatic));
    next.opponents.reset();
    next.opponentFrame.reset();
    CHECK(encode(next).size() == CombatPacket::kMaxBytes);
    auto chunks = packets(next);
    REQUIRE(chunks.size() == CombatReplica::kMaxChunks);
    REQUIRE(chunks.size() > 200);
    std::ranges::reverse(chunks);
    for (usize i = 0; i + 1 < chunks.size(); ++i) {
        CHECK(replica.receive(1, chunks[i]) == Admission::Pending);
        CHECK(replica.receive(1, chunks[i]) == Admission::Duplicate);
        REQUIRE(replica.latest());
        CHECK(replica.latest()->motion.tick == 1);
        CHECK(replica.latest()->enemies.empty());
    }
    CHECK(replica.receive(1, chunks.back()) == Admission::Committed);
    REQUIRE(replica.latest());
    CHECK(encode(*replica.latest()) == encode(next));
    CHECK(replica.pending() == 0);
    CHECK(replica.receive(1, chunks.back()) == Admission::Stale);
    CombatReplica repaired;
    REQUIRE(repaired.begin(1, 9));
    const auto repair = CombatReplica::packets(next, SnapshotBlock::Compression::None,
                                               CombatReplica::Recovery::SingleLoss);
    REQUIRE(repair);
    REQUIRE(repair->size() > 255);
    for (usize i = 0; i < repair->size(); ++i) {
        if (i < chunks.size() && i % CombatReplica::kRepairGroup == 0) {
            continue;
        }
        CHECK(repaired.receive(1, (*repair)[i]) != Admission::Invalid);
    }
    REQUIRE(repaired.latest());
    CHECK(encode(*repaired.latest()) == encode(next));
}

TEST_CASE("projectile records round trip their resource namespaces and reject corrupt transforms",
          "[netplay][combat-snapshot]") {
    auto source = state(3, 0);
    for (u32 kind = 0; kind <= static_cast<u32>(ProjectileSource::PlayerRider); ++kind) {
        ProjectileState shot;
        shot.source = static_cast<ProjectileSource>(kind);
        shot.instance = 0x123456789ABCDEF0ULL;
        shot.resource = kind + 1;
        shot.placement = glm::rotate(glm::translate(Mat4{1}, Vec3{1, 2, 3}), 0.5f, Vec3{0, 1, 0});
        shot.direction = {3, 5, 7};
        shot.age = 0.75f;
        shot.radius = 1.5f;
        shot.forward = -0.5f;
        shot.tint = Color{0x12, 0x34, 0x56, 0x78};
        shot.animation = {0, 1, 7, 2.5f, 1};
        shot.textureFrame = 50.5f;
        shot.alpha = 0.75f;
        shot.flags = 31;
        source.projectiles.push_back(shot);
    }
    const auto valid = encode(source);
    const auto decoded = CombatPacket::decode(valid);
    REQUIRE(decoded);
    CHECK(encode(*decoded) == valid);
    constexpr usize kShot = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                            InputCommand::kSeats * CombatPacket::kPlayerBytes;
    for (usize size = kShot; size < valid.size(); ++size) {
        CHECK_FALSE(CombatPacket::decode(std::span(valid).first(size)));
    }
    for (const auto [offset, value] : std::array<std::pair<usize, u32>, 10>{
             {{4, 1},
              {12, static_cast<u32>(CombatSnapshot::kMaxProjectiles + 1)},
              {kShot, 6},
              {kShot + 12, 0},
              {kShot + 16, 0},
              {kShot + 144, 32},
              {kShot + 32, 0x3F800000},
              {kShot + 80, 0},
              {kShot + 16, 65536},
              {kShot, 256}}}) {
        auto bytes = valid;
        writeWord(bytes, offset, value);
        CHECK_FALSE(CombatPacket::decode(bytes));
    }
    for (const usize offset : {20U, 68U, 84U, 88U, 92U, 96U, 100U, 104U, 128U, 132U, 136U, 140U}) {
        auto bytes = valid;
        writeWord(bytes, kShot + offset, 0x7FC00000);
        CHECK_FALSE(CombatPacket::decode(bytes));
    }
    auto invalid = source;
    invalid.projectiles[1] = invalid.projectiles[0];
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    std::swap(invalid.projectiles[0], invalid.projectiles[1]);
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    invalid.projectiles[0].instance = 0;
    CHECK_FALSE(CombatPacket::encode(invalid));
}

TEST_CASE("projectile rollback preserves complete release identities and native flight state",
          "[netplay][combat-snapshot][rollback]") {
    auto source = rollbackShotState();
    source.motion.epoch = 0x123456789ABCDEF0ULL;
    source.motion.tick = 0x23456789ABCDEF01ULL;
    for (const auto kind : {ProjectileSource::Player, ProjectileSource::PlayerEffect,
                            ProjectileSource::Streak, ProjectileSource::PlayerRider}) {
        source.projectiles.front().source = kind;
        for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
            for (const bool strong : {false, true}) {
                auto& expected = *source.projectiles.front().rollback;
                expected.origin = {source.motion.epoch,
                                   source.motion.tick - 1,
                                   0x3456789ABCDEF012ULL,
                                   0x12345678,
                                   seat,
                                   strong};
                source.motion.players[seat]->grant = expected.origin.grant;
                expected.held = strong;
                const auto bytes = encode(source);
                const auto decoded = CombatPacket::decode(bytes);
                REQUIRE(decoded);
                REQUIRE(decoded->projectiles.size() == 1);
                REQUIRE(decoded->projectiles.front().rollback);
                const auto& actual = *decoded->projectiles.front().rollback;
                CHECK(actual.origin == expected.origin);
                CHECK(actual.character == expected.character);
                CHECK(actual.flight == expected.flight);
                CHECK(actual.held == expected.held);
                CHECK(encode(*decoded) == bytes);
            }
        }
    }
}

TEST_CASE("turbo identities survive live projectiles and retired particle tails",
          "[netplay][combat-snapshot][turbo-volley-identity]") {
    for (const bool retiring : {false, true}) {
        for (const u32 emission : {0x00010000U, 0x00010001U, 0x00020000U, 0x0080FFFFU}) {
            auto source = particleShotState(retiring);
            auto& shot = source.projectiles.front();
            shot.particles->origin->emission = emission;
            if (shot.rollback) {
                shot.rollback->origin.emission = emission;
            }
            const auto bytes = encode(source);
            const auto decoded = CombatPacket::decode(bytes);
            REQUIRE(decoded);
            REQUIRE(decoded->projectiles.front().releaseOrigin());
            CHECK(decoded->projectiles.front().releaseOrigin()->emission == emission);
            CHECK(decoded->projectiles.front().particles->origin == shot.particles->origin);
            if (!retiring) {
                CHECK(decoded->projectiles.front().rollback->origin == shot.rollback->origin);
            }
            CHECK(encode(*decoded) == bytes);
        }
    }
    const auto source = particleShotState();
    const auto valid = encode(source);
    constexpr usize kShot = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                            InputCommand::kSeats * CombatPacket::kPlayerBytes;
    constexpr usize kRollback = kShot + 148;
    constexpr usize kParticles = kRollback + CombatPacket::kShotRollbackBytes;
    for (const u32 emission : {1U, 65535U, 0x00810000U, 0xFFFFFFFFU}) {
        auto bytes = valid;
        writeWord(bytes, kRollback + 80, emission);
        writeWord(bytes, kParticles + 92, emission);
        CHECK_FALSE(CombatPacket::decode(bytes));
    }
    auto mismatch = valid;
    writeWord(mismatch, kRollback + 80, 0x00010000U);
    writeWord(mismatch, kParticles + 92, 0x00010001U);
    CHECK_FALSE(CombatPacket::decode(mismatch));
    for (const u32 flags : {256U, 1U << 16U}) {
        auto bytes = valid;
        writeWord(bytes, kRollback + 32, flags);
        writeWord(bytes, kRollback + 80, 0x00010000U);
        writeWord(bytes, kParticles + 88, flags);
        writeWord(bytes, kParticles + 92, 0x00010000U);
        CHECK_FALSE(CombatPacket::decode(bytes));
    }
}

TEST_CASE("projectile rollback rejects malformed ownership and out of bounds flight state",
          "[netplay][combat-snapshot][rollback]") {
    const auto source = rollbackShotState();
    const auto bytes = encode(source);
    constexpr usize kShot = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                            InputCommand::kSeats * CombatPacket::kPlayerBytes;
    constexpr usize kRollback = kShot + CombatPacket::kProjectileBytes -
                                CombatPacket::kShotRollbackBytes - CombatPacket::kParticleBytes -
                                CombatPacket::kEffectOriginBytes -
                                CombatPacket::kPotionRollbackBytes;
    for (const usize offset : {40U, 44U, 48U, 52U, 56U, 60U, 64U, 68U, 72U}) {
        for (const u32 value : {0x7FC00000U, 0x7F800000U, 0xFF800000U}) {
            auto corrupt = bytes;
            writeWord(corrupt, kRollback + offset, value);
            CHECK_FALSE(CombatPacket::decode(corrupt));
        }
    }
    for (const auto [offset, value] : std::array<std::pair<usize, u32>, 12>{{{0, 2},
                                                                             {4, 0},
                                                                             {4, 10},
                                                                             {20, 0},
                                                                             {28, 0},
                                                                             {28, 2},
                                                                             {32, 4},
                                                                             {32, 1024},
                                                                             {36, 17},
                                                                             {76, 0},
                                                                             {32, 0xFFFFFFFF},
                                                                             {36, 0xFFFFFFFF}}}) {
        auto corrupt = bytes;
        writeWord(corrupt, kRollback + offset, value);
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    const std::array<std::pair<usize, f32>, 14> outside{{{40, -1000001.0f},
                                                         {44, 1000001.0f},
                                                         {48, 1000001.0f},
                                                         {52, -10001.0f},
                                                         {56, 10001.0f},
                                                         {60, 10001.0f},
                                                         {64, -10001.0f},
                                                         {64, 10001.0f},
                                                         {68, -0.01f},
                                                         {68, PlayerMissiles::kLifeSeconds},
                                                         {72, 0.009f},
                                                         {72, 16.01f},
                                                         {40, 1000001.0f},
                                                         {52, 10001.0f}}};
    for (const auto [offset, value] : outside) {
        auto corrupt = bytes;
        writeWord(corrupt, kRollback + offset, std::bit_cast<u32>(value));
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    for (const bool upper : {false, true}) {
        auto bounded = source;
        auto& rollback = *bounded.projectiles.front().rollback;
        rollback.character = upper ? 16 : 0;
        rollback.flight.position = Vec3{upper ? 1000000.0f : -1000000.0f};
        rollback.flight.velocity = Vec3{upper ? 10000.0f : -10000.0f};
        rollback.flight.tumble = upper ? 10000.0f : -10000.0f;
        rollback.flight.age = upper ? std::nextafter(PlayerMissiles::kLifeSeconds, 0.0f) : 0;
        rollback.flight.scale = upper ? 16.0f : 0.01f;
        CHECK(CombatPacket::decode(encode(bounded)));
    }
    for (const auto kind : {ProjectileSource::Enemy, ProjectileSource::WorldEffect,
                            ProjectileSource::Arrival, ProjectileSource::TowerWizard}) {
        auto unsupported = source;
        unsupported.projectiles.front().source = kind;
        CHECK_FALSE(CombatPacket::encode(unsupported));
        auto corrupt = bytes;
        writeWord(corrupt, kShot, static_cast<u32>(kind));
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    auto invalid = source;
    invalid.projectiles.front().rollback->origin.seat = 4;
    CHECK_FALSE(invalid.valid()); // check the seat before looking up its owner
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    invalid.motion.players[0].reset();
    invalid.players[0].reset();
    CHECK_FALSE(invalid.valid()); // no dereference of a departed owner
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    ++invalid.projectiles.front().rollback->origin.grant;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    ++invalid.projectiles.front().rollback->origin.epoch;
    CHECK_FALSE(CombatPacket::encode(invalid));
}

TEST_CASE("absent projectile rollback data is canonical and future releases stay off the wire",
          "[netplay][combat-snapshot][rollback]") {
    auto source = rollbackShotState();
    constexpr usize kRollback = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                                InputCommand::kSeats * CombatPacket::kPlayerBytes +
                                CombatPacket::kProjectileBytes - CombatPacket::kShotRollbackBytes -
                                CombatPacket::kParticleBytes - CombatPacket::kEffectOriginBytes -
                                CombatPacket::kPotionRollbackBytes;
    const auto bytes = encode(source);
    auto future = source;
    future.projectiles.front().rollback->origin.tick = source.motion.tick + 1;
    CHECK(future.valid()); // mixed-age local presentation may lead the host checkpoint
    CHECK_FALSE(CombatPacket::encode(future));
    auto corrupt = bytes;
    writeWord(corrupt, kRollback + 12, static_cast<u32>(source.motion.tick + 1));
    CHECK_FALSE(CombatPacket::decode(corrupt));
    source.projectiles.front().rollback.reset();
    const auto absent = encode(source);
    const auto decoded = CombatPacket::decode(absent);
    REQUIRE(decoded);
    REQUIRE(decoded->projectiles.size() == 1);
    CHECK_FALSE(decoded->projectiles.front().rollback);
    for (usize offset = 0; offset < CombatPacket::kShotRollbackBytes; offset += 4) {
        corrupt = absent;
        writeWord(corrupt, kRollback + offset, 1);
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    corrupt = bytes;
    writeWord(corrupt, kRollback, 0);
    CHECK_FALSE(CombatPacket::decode(corrupt));
}

TEST_CASE("particle histories retain native birth cadence terminal placement and release identity",
          "[netplay][combat-snapshot][particle-flight]") {
    for (const bool retiring : {false, true}) {
        auto source = particleShotState(retiring);
        source.motion.epoch = 0x123456789ABCDEF0ULL;
        source.motion.tick = 0x23456789ABCDEF01ULL;
        auto& shot = source.projectiles.front();
        shot.placement =
            glm::rotate(glm::translate(Mat4{1}, Vec3{23, -7, 41}), 0.7f, Vec3{0, 1, 0});
        shot.direction = {-5, 2, 17};
        for (const auto kind : {ProjectileSource::PlayerEffect, ProjectileSource::PlayerRider}) {
            shot.source = kind;
            for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
                for (u8 ordinal = 0; ordinal < ShotIdentity::kMaxShots; ++ordinal) {
                    for (const bool strong : {false, true}) {
                        auto& expected = *shot.particles;
                        expected.origin = ShotIdentity{source.motion.epoch,
                                                       source.motion.tick - 20,
                                                       0x3456789ABCDEF012ULL,
                                                       0x12345678,
                                                       seat,
                                                       strong,
                                                       ordinal};
                        source.motion.players[seat]->grant = expected.origin->grant;
                        if (shot.rollback) {
                            shot.rollback->origin = *expected.origin;
                        }
                        const auto bytes = encode(source);
                        const auto decoded = CombatPacket::decode(bytes);
                        REQUIRE(decoded);
                        REQUIRE(decoded->projectiles.size() == 1);
                        const auto& actual = decoded->projectiles.front();
                        REQUIRE(actual.particles);
                        CHECK(actual.particles->character == expected.character);
                        CHECK(actual.particles->flight == expected.flight);
                        CHECK(actual.particles->origin == expected.origin);
                        CHECK(actual.placement == shot.placement);
                        CHECK(actual.direction == shot.direction);
                        CHECK(actual.rollback.has_value() == !retiring);
                        REQUIRE(actual.releaseOrigin());
                        CHECK(*actual.releaseOrigin() == *expected.origin);
                        CHECK(encode(*decoded) == bytes);
                    }
                }
            }
        }
    }
}

TEST_CASE("particle histories reject malformed flags ownership and nonfinite fields",
          "[netplay][combat-snapshot][particle-flight]") {
    const auto source = particleShotState(true);
    const auto bytes = encode(source);
    constexpr usize kShot = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                            InputCommand::kSeats * CombatPacket::kPlayerBytes;
    constexpr usize kParticles = kShot + CombatPacket::kProjectileBytes -
                                 CombatPacket::kParticleBytes - CombatPacket::kEffectOriginBytes -
                                 CombatPacket::kPotionRollbackBytes;
    for (const usize offset : {8U, 12U, 16U, 20U, 24U, 28U, 32U, 36U, 40U, 48U}) {
        for (const u32 value : {0x7FC00000U, 0x7F800000U, 0xFF800000U}) {
            auto corrupt = bytes;
            writeWord(corrupt, kParticles + offset, value);
            CHECK_FALSE(CombatPacket::decode(corrupt));
        }
    }
    for (const auto [offset, value] : std::array<std::pair<usize, u32>, 23>{
             {{0, 2},           {0, 4},          {0, 6},           {0, 8},   {0, 16},
              {4, 17},          {44, 0},         {44, 2},          {52, 21}, {52, 181},
              {52, 0xFFFFFFFF}, {56, 1202},      {56, 0xFFFFFFFF}, {60, 0},  {60, 10},
              {76, 0},          {84, 0},         {84, 2},          {88, 4},  {88, 512},
              {88, 5U << 16U},  {88, 1U << 19U}, {88, 0xFFFFFFFF}}}) {
        auto corrupt = bytes;
        writeWord(corrupt, kParticles + offset, value);
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    for (const auto kind :
         {ProjectileSource::Player, ProjectileSource::Enemy, ProjectileSource::WorldEffect,
          ProjectileSource::Streak, ProjectileSource::Arrival, ProjectileSource::TowerWizard}) {
        auto invalid = source;
        invalid.projectiles.front().source = kind;
        CHECK_FALSE(CombatPacket::encode(invalid));
        auto corrupt = bytes;
        writeWord(corrupt, kShot, static_cast<u32>(kind));
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    auto invalid = source;
    invalid.projectiles.front().particles->origin->seat = 4;
    CHECK_FALSE(invalid.valid()); // validate before indexing the seat array
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    invalid.motion.players[0].reset();
    invalid.players[0].reset();
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    ++invalid.projectiles.front().particles->origin->epoch;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    ++invalid.projectiles.front().particles->origin->grant;
    CHECK_FALSE(CombatPacket::encode(invalid));
    for (usize size = kParticles; size < bytes.size(); ++size) {
        CHECK_FALSE(CombatPacket::decode(std::span(bytes).first(size)));
    }
}

TEST_CASE("particle holds cover live and retired effects without advancing history",
          "[netplay][combat-snapshot][particle-flight]") {
    for (const bool retiring : {false, true}) {
        for (const bool held : {false, true}) {
            auto source = particleShotState(retiring);
            auto& shot = source.projectiles.front();
            shot.particles->held = held;
            if (shot.rollback) {
                shot.rollback->held = held;
            }
            const auto bytes = encode(source);
            const auto decoded = CombatPacket::decode(bytes);
            REQUIRE(decoded);
            REQUIRE(decoded->projectiles.front().particles);
            CHECK(decoded->projectiles.front().particles->held == held);
            CHECK(decoded->projectiles.front().particles->flight == shot.particles->flight);
            CHECK(encode(*decoded) == bytes);
            if (shot.rollback) {
                shot.rollback->held = !held;
                CHECK_FALSE(CombatPacket::encode(source));
            }
        }
    }
    auto source = particleShotState(false);
    auto corrupt = encode(source);
    constexpr usize kParticles = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                                 InputCommand::kSeats * CombatPacket::kPlayerBytes +
                                 CombatPacket::kProjectileBytes - CombatPacket::kParticleBytes -
                                 CombatPacket::kEffectOriginBytes -
                                 CombatPacket::kPotionRollbackBytes;
    writeWord(corrupt, kParticles, 11); // live history held, physical flight not held
    CHECK_FALSE(CombatPacket::decode(corrupt));
}

TEST_CASE("particle histories bound birth state and native emitting and retirement clocks",
          "[netplay][combat-snapshot][particle-flight]") {
    const auto source = particleShotState(true);
    const auto bytes = encode(source);
    constexpr usize kParticles = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                                 InputCommand::kSeats * CombatPacket::kPlayerBytes +
                                 CombatPacket::kProjectileBytes - CombatPacket::kParticleBytes -
                                 CombatPacket::kEffectOriginBytes -
                                 CombatPacket::kPotionRollbackBytes;
    for (const auto [offset, value] :
         std::array<std::pair<usize, f32>, 15>{{{8, -1000001.0f},
                                                {12, 1000001.0f},
                                                {16, 1000001.0f},
                                                {20, -10001.0f},
                                                {24, 10001.0f},
                                                {28, 10001.0f},
                                                {32, 0.01f},
                                                {36, 0.01f},
                                                {40, 0.0f},
                                                {40, -0.01f},
                                                {40, 100.01f},
                                                {48, 0.0f},
                                                {48, -0.01f},
                                                {48, std::nextafter(1.0f / 240, 0.0f)},
                                                {48, std::nextafter(0.25f, 1.0f)}}}) {
        auto corrupt = bytes;
        writeWord(corrupt, kParticles + offset, std::bit_cast<u32>(value));
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    for (const f32 seconds : {1.0f / 240, 1.0f / 60, 0.25f}) {
        for (const bool retiring : {false, true}) {
            auto bounded = particleShotState(retiring);
            auto& particles = *bounded.projectiles.front().particles;
            particles.character = 16;
            auto& flight = particles.flight;
            flight.birth.position = Vec3{1000000.0f, -1000000.0f, 0};
            flight.birth.velocity = Vec3{-10000.0f, 10000.0f, 0};
            flight.birth.scale = 100;
            flight.stepSeconds = seconds;
            flight.activeSteps =
                static_cast<u32>(std::ceil(PlayerMissiles::kLifeSeconds / seconds));
            flight.totalSteps =
                retiring ? static_cast<u32>(std::ceil(20.0f / seconds)) + 1 : flight.activeSteps;
            if (bounded.projectiles.front().rollback) {
                bounded.projectiles.front().rollback->character = particles.character;
            }
            CHECK(CombatPacket::decode(encode(bounded)));
            auto invalid = bounded;
            ++invalid.projectiles.front().particles->flight.activeSteps;
            CHECK_FALSE(CombatPacket::encode(invalid));
            invalid = bounded;
            ++invalid.projectiles.front().particles->flight.totalSteps;
            CHECK_FALSE(CombatPacket::encode(invalid));
        }
    }
    auto empty = particleShotState(false);
    empty.projectiles.front().particles->flight.activeSteps = 0;
    empty.projectiles.front().particles->flight.totalSteps = 0;
    CHECK(CombatPacket::decode(encode(empty)));
    empty.projectiles.front().particles->flight.retiring = true;
    empty.projectiles.front().rollback.reset();
    CHECK_FALSE(CombatPacket::encode(empty));
    empty.projectiles.front().particles->flight.totalSteps = 1;
    CHECK(CombatPacket::decode(encode(empty))); // impact before the first emitting update
}

TEST_CASE("particle tails cannot carry live rollback or mismatched release descriptions",
          "[netplay][combat-snapshot][particle-flight]") {
    auto source = particleShotState(false);
    auto invalid = source;
    ++invalid.projectiles.front().particles->character;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    ++invalid.projectiles.front().particles->origin->generation;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    invalid.projectiles.front().particles->flight.retiring = true;
    ++invalid.projectiles.front().particles->flight.totalSteps;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid.projectiles.front().rollback.reset();
    CHECK(CombatPacket::decode(encode(invalid)));
    const auto origin = *source.projectiles.front().releaseOrigin();
    source.projectiles.front().particles->origin.reset();
    CHECK(CombatPacket::decode(encode(source)));
    REQUIRE(source.projectiles.front().releaseOrigin());
    CHECK(*source.projectiles.front().releaseOrigin() == origin);
    source.projectiles.front().rollback.reset();
    CHECK_FALSE(source.projectiles.front().releaseOrigin());
    CHECK(CombatPacket::decode(encode(source)));
    source.projectiles.front().particles.reset();
    CHECK_FALSE(source.projectiles.front().releaseOrigin());
}

TEST_CASE("absent particle data is canonical and future tail origins stay off the wire",
          "[netplay][combat-snapshot][particle-flight]") {
    auto source = particleShotState(true);
    constexpr usize kParticles = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                                 InputCommand::kSeats * CombatPacket::kPlayerBytes +
                                 CombatPacket::kProjectileBytes - CombatPacket::kParticleBytes -
                                 CombatPacket::kEffectOriginBytes -
                                 CombatPacket::kPotionRollbackBytes;
    const auto bytes = encode(source);
    auto future = source;
    future.projectiles.front().particles->origin->tick = source.motion.tick + 1;
    CHECK(future.valid());
    CHECK_FALSE(CombatPacket::encode(future));
    auto corrupt = bytes;
    writeWord(corrupt, kParticles + 68, static_cast<u32>(source.motion.tick + 1));
    CHECK_FALSE(CombatPacket::decode(corrupt));
    source.projectiles.front().particles->origin.reset();
    const auto noOrigin = encode(source);
    const auto decoded = CombatPacket::decode(noOrigin);
    REQUIRE(decoded);
    REQUIRE(decoded->projectiles.front().particles);
    CHECK_FALSE(decoded->projectiles.front().particles->origin);
    for (usize offset = 60; offset < CombatPacket::kParticleBytes; ++offset) {
        corrupt = noOrigin;
        corrupt[kParticles + offset] = 1;
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    source.projectiles.front().particles.reset();
    const auto absent = encode(source);
    for (usize offset = 0; offset < CombatPacket::kParticleBytes; ++offset) {
        corrupt = absent;
        corrupt[kParticles + offset] = 1;
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    corrupt = bytes;
    writeWord(corrupt, kParticles, 0);
    CHECK_FALSE(CombatPacket::decode(corrupt));
    corrupt = bytes;
    corrupt[4] = 31;
    CHECK_FALSE(CombatPacket::decode(corrupt));
}

TEST_CASE("potion particle histories preserve native kind and identity across physical retirement",
          "[netplay][combat-snapshot][particle-flight][potion-rollback]") {
    for (const bool retiring : {false, true}) {
        for (const bool held : {false, true}) {
            for (u8 kind = 1; kind <= 4; ++kind) {
                for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
                    auto source = particlePotionState(retiring);
                    auto& shot = source.projectiles.front();
                    auto& particles = *shot.particles;
                    particles.held = held;
                    particles.potion = kind;
                    particles.potionOrigin->seat = seat;
                    particles.potionOrigin->generation = 0x123456789ABCDEF0ULL;
                    if (shot.potion) {
                        shot.potion->held = held;
                        shot.potion->kind = kind;
                        shot.potion->origin = *particles.potionOrigin;
                    }
                    const auto bytes = encode(source);
                    const auto decoded = CombatPacket::decode(bytes);
                    REQUIRE(decoded);
                    const auto& actual = decoded->projectiles.front();
                    REQUIRE(actual.particles);
                    CHECK(actual.particles->character == 0);
                    CHECK(actual.particles->potion == kind);
                    CHECK(actual.particles->potionOrigin == particles.potionOrigin);
                    CHECK(actual.particles->held == held);
                    CHECK(actual.particles->flight == particles.flight);
                    CHECK(actual.potion.has_value() == !retiring);
                    CHECK_FALSE(actual.releaseOrigin());
                    REQUIRE(actual.potionReleaseOrigin());
                    CHECK(*actual.potionReleaseOrigin() == *particles.potionOrigin);
                    CHECK(encode(*decoded) == bytes);
                }
            }
        }
    }
}

TEST_CASE(
    "potion particle packets reject ambiguous origins malformed kinds and noncanonical padding",
    "[netplay][combat-snapshot][particle-flight][potion-rollback]") {
    auto source = particlePotionState(true);
    const auto bytes = encode(source);
    constexpr usize kParticles = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                                 InputCommand::kSeats * CombatPacket::kPlayerBytes + 148 +
                                 CombatPacket::kShotRollbackBytes;
    for (const auto [offset, value] : std::array<std::pair<usize, u32>, 18>{{{0, 16},
                                                                             {0, 23},
                                                                             {0, 53},
                                                                             {4, 0},
                                                                             {4, 3U << 8U | 1U},
                                                                             {4, 5U << 8U},
                                                                             {4, 1U << 11U},
                                                                             {60, 0},
                                                                             {60, 10},
                                                                             {68, 81},
                                                                             {76, 0},
                                                                             {84, 0},
                                                                             {84, 2},
                                                                             {88, 0},
                                                                             {88, 260},
                                                                             {88, 512},
                                                                             {88, 1U << 16U},
                                                                             {92, 1}}}) {
        auto corrupt = bytes;
        writeWord(corrupt, kParticles + offset, value);
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
    for (usize size = kParticles; size < kParticles + CombatPacket::kParticleBytes; ++size) {
        CHECK_FALSE(CombatPacket::decode(std::span(bytes).first(size)));
    }
    for (usize part = 0; part < 8; ++part) {
        auto invalid = source;
        auto& shot = invalid.projectiles.front();
        switch (part) {
        case 0: shot.particles->origin = ShotIdentity{9, 75, 12, 1, 0, false}; break;
        case 1: shot.particles->potion = 0; break;
        case 2: shot.particles->character = 1; break;
        case 3: shot.particles->potionOrigin->seat = 4; break;
        case 4: shot.source = ProjectileSource::PlayerRider; break;
        case 5: shot.particles->potionOrigin->kind = PotionIdentity::Kind::Use; break;
        case 6:
            invalid.motion.players[0].reset();
            invalid.players[0].reset();
            break;
        case 7: shot.rollback = rollbackShotState().projectiles.front().rollback; break;
        }
        CHECK_FALSE(invalid.valid());
        CHECK_FALSE(CombatPacket::encode(invalid));
    }
    auto future = source;
    future.projectiles.front().particles->potionOrigin->tick = source.motion.tick + 1;
    CHECK(future.valid());
    CHECK_FALSE(CombatPacket::encode(future));
    auto duplicate = source.projectiles.front();
    ++duplicate.instance;
    source.projectiles.push_back(duplicate);
    CHECK_FALSE(source.valid());
    source.projectiles.pop_back();
    source.projectiles.front().particles->potionOrigin.reset();
    CHECK_FALSE(source.projectiles.front().potionReleaseOrigin());
    const auto noOrigin = encode(source);
    REQUIRE(CombatPacket::decode(noOrigin));
    for (usize offset = 60; offset < CombatPacket::kParticleBytes; ++offset) {
        auto corrupt = noOrigin;
        corrupt[kParticles + offset] = 1;
        CHECK_FALSE(CombatPacket::decode(corrupt));
    }
}

TEST_CASE("live potion flight and native particle history agree on kind ownership and hold",
          "[netplay][combat-snapshot][particle-flight][potion-rollback]") {
    const auto source = particlePotionState(false);
    for (usize part = 0; part < 6; ++part) {
        auto invalid = source;
        auto& shot = invalid.projectiles.front();
        switch (part) {
        case 0: ++shot.potion->kind; break;
        case 1: shot.potion->held = true; break;
        case 2: ++shot.potion->origin.generation; break;
        case 3: shot.particles->potionOrigin.reset(); break;
        case 4:
            shot.particles->flight.retiring = true;
            ++shot.particles->flight.totalSteps;
            break;
        case 5: shot.particles->potion = 0; break;
        }
        CHECK_FALSE(invalid.valid());
        CHECK_FALSE(CombatPacket::encode(invalid));
    }
}

TEST_CASE("geometry codecs reject malformed lengths indices flags and matrices",
          "[netplay][combat-snapshot][geometry-replica]") {
    auto source = state(3, 0);
    source.geometry = SceneGeometry{0x123456789ABCDEF0ULL, 5, 0.25f, {{0}, {3}}};
    source.geometry->objects[0].local =
        glm::rotate(glm::translate(Mat4{1}, Vec3{2, 3, 4}), 0.5f, Vec3{0, 1, 0});
    source.geometry->objects[1].alpha = 0.5f;
    source.geometry->objects[1].visible = false;
    const auto valid = encode(source);
    const auto decoded = CombatPacket::decode(valid);
    REQUIRE(decoded);
    CHECK(encode(*decoded) == valid);
    constexpr usize kGeometry = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                                InputCommand::kSeats * CombatPacket::kPlayerBytes;
    constexpr usize kObject = kGeometry + CombatPacket::kGeometryHeaderBytes;
    for (usize size = kGeometry; size < valid.size(); ++size) {
        CHECK_FALSE(CombatPacket::decode(std::span(valid).first(size)));
    }
    const std::array<std::pair<usize, u32>, 13> corruptions{
        {{16, 1},
         {16, 0xFFFFFFFF},
         {kGeometry + 8, 0},
         {kGeometry + 8, 65536},
         {kGeometry + 12, 2049},
         {kGeometry + 12, 1},
         {kGeometry + 16, 0x7FC00000},
         {kObject, 5},
         {kObject + 4, 0},
         {kObject + 8, 0x7F800000},
         {kObject + 56, 0x40000000},
         {kObject + 60, 2},
         {kObject + CombatPacket::kGeometryObjectBytes, 0}}};
    for (const auto [offset, value] : corruptions) {
        auto bytes = valid;
        writeWord(bytes, offset, value);
        CHECK_FALSE(CombatPacket::decode(bytes));
    }
    auto invalid = source;
    invalid.geometry->objects[0].local[0].w = 1;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    invalid.geometry->objects[0].local[3].x = 1'000'001;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    invalid.geometry->objects.resize(SceneGeometry::kMaxStates + 1);
    CHECK_FALSE(CombatPacket::encode(invalid));
}

TEST_CASE("lost combat fragments cannot resurrect a removed or recycled enemy",
          "[netplay][combat-snapshot]") {
    CombatReplica replica;
    REQUIRE(replica.begin(1, 9));
    commit(replica, state(1));
    auto dying = state(2);
    dying.enemies[0].health = 0;
    dying.enemies[0].life = ReplicaEnemyLife::Dying;
    const auto missing = packets(dying);
    REQUIRE(missing.size() > 1);
    CHECK(replica.receive(1, missing[0]) == Admission::Pending);
    auto replaced = state(3);
    replaced.enemies.erase(replaced.enemies.begin());
    auto born = replaced.enemies.back();
    ++born.instance;
    replaced.enemies.push_back(born);
    replaced.players[0]->health = 0;
    replaced.players[0]->life = ReplicaPlayerLife::Dying;
    commit(replica, replaced);
    for (const auto& chunk : missing) {
        CHECK(replica.receive(1, chunk) == Admission::Stale);
    }
    CHECK(replica.latest()->enemies.front().instance == 2);
    CHECK(replica.latest()->enemies.back().instance == 26);
    CHECK(replica.latest()->players[0]->health == 0);
}

TEST_CASE("combat fragment validation and host epochs bound partial state",
          "[netplay][combat-snapshot]") {
    CombatReplica replica;
    const auto chunks = packets(state(1));
    CHECK(replica.receive(1, chunks[0]) == Admission::WrongHost);
    CHECK_FALSE(replica.begin(0, 9));
    CHECK_FALSE(replica.begin(1, 0));
    REQUIRE(replica.begin(1, 9));
    CHECK(replica.receive(2, chunks[0]) == Admission::WrongHost);
    for (usize size = 0; size < chunks[0].size(); ++size) {
        CHECK(replica.receive(1, std::span(chunks[0]).first(size)) == Admission::Invalid);
    }
    for (const usize offset : {0U, 4U, 5U, 6U, 7U}) {
        auto bad = chunks[0];
        bad[offset] = 255;
        CHECK(replica.receive(1, bad) == Admission::Invalid);
    }
    auto oversized = chunks[0];
    writeWord(oversized, 24, static_cast<u32>(SnapshotBlock::kMaxBytes + 1));
    CHECK(replica.receive(1, oversized) == Admission::Invalid);
    for (u64 tick = 1; tick <= 10; ++tick) {
        CHECK(replica.receive(1, packets(state(tick))[0]) == Admission::Pending);
        CHECK(replica.pending() <= CombatReplica::kPendingSnapshots);
    }
    CHECK(replica.receive(1, chunks[0]) == Admission::Stale);
    CHECK_FALSE(replica.latest());
    CHECK_FALSE(replica.begin(2, 10));
    CHECK_FALSE(replica.begin(1, 9));
    REQUIRE(replica.begin(1, 10));
    CHECK(replica.pending() == 0);
    CHECK(replica.receive(1, chunks[0]) == Admission::WrongEpoch);
    replica.clear();
    CHECK_FALSE(replica.latest());
    REQUIRE(replica.begin(2, 1));
}

TEST_CASE("different captures of one tick and corrupted chunks cannot be combined",
          "[netplay][combat-snapshot]") {
    CombatReplica replica;
    REQUIRE(replica.begin(1, 9));
    auto snapshot = state(1);
    const auto first = packets(snapshot);
    snapshot.players[0]->health -= 20;
    const auto other = packets(snapshot);
    CHECK(replica.receive(1, first[0]) == Admission::Pending);
    CHECK(replica.receive(1, other[1]) == Admission::Invalid);
    auto damaged = first[0];
    damaged.back() ^= 1;
    CHECK(replica.receive(1, damaged) == Admission::Invalid);
    for (usize i = 1; i < first.size(); ++i) {
        CHECK(replica.receive(1, first[i]) ==
              (i + 1 == first.size() ? Admission::Committed : Admission::Pending));
    }
    auto corrupt = packets(state(2));
    corrupt[0].back() ^= 1; // valid-sized payload, invalid whole-state digest
    for (usize i = 0; i < corrupt.size(); ++i) {
        CHECK(replica.receive(1, corrupt[i]) ==
              (i + 1 == corrupt.size() ? Admission::Invalid : Admission::Pending));
    }
    REQUIRE(replica.latest());
    CHECK(replica.latest()->motion.tick == 1);
    commit(replica, state(2)); // clean retransmission recovers
}
} // namespace
