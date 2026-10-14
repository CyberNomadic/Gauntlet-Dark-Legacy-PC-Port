#include <algorithm>
#include <bit>
#include <chrono>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <lz4.h>

#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/CombatStatePacket.h"
#include "game/netplay/StageStatePacket.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Packet = StageStatePacket;

void word(std::vector<u8>& bytes, usize offset, u32 value) {
    REQUIRE(offset + 4 <= bytes.size());
    for (usize index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<u8>(value >> (index * 8));
    }
}
u32 word(const std::vector<u8>& bytes, usize offset) {
    REQUIRE(offset + 4 <= bytes.size());
    u32 result = 0;
    for (usize index = 0; index < 4; ++index) {
        result |= static_cast<u32>(bytes[offset + index]) << (index * 8);
    }
    return result;
}
auto raw(const Packet::State& state, const Packet::Catalog& catalog) {
    return Packet::encode(state, catalog, Packet::Encoding::Raw);
}
void rawLengths(std::vector<u8>& bytes) {
    REQUIRE(bytes.size() >= Packet::kHeaderBytes);
    REQUIRE(word(bytes, 12) == 0);
    word(bytes, 8, static_cast<u32>(bytes.size()));
    word(bytes, 16, static_cast<u32>(bytes.size() - Packet::kHeaderBytes));
}
// Repack deliberately altered raw fields to exercise the same nested validators
// behind decompression, not merely a malformed compression header.
std::vector<u8> pack(const std::vector<u8>& bytes) {
    REQUIRE(word(bytes, 12) == 0);
    REQUIRE(bytes.size() > Packet::kHeaderBytes);
    const auto size = static_cast<s32>(bytes.size() - Packet::kHeaderBytes);
    std::vector<u8> result(Packet::kHeaderBytes + static_cast<usize>(LZ4_compressBound(size)));
    std::copy_n(bytes.begin(), Packet::kHeaderBytes, result.begin());
    // LZ4 char pointers address byte buffers only.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    const s32 written =
        LZ4_compress_default(reinterpret_cast<const char*>(bytes.data() + Packet::kHeaderBytes),
                             reinterpret_cast<char*>(result.data() + Packet::kHeaderBytes), size,
                             static_cast<s32>(result.size() - Packet::kHeaderBytes));
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    REQUIRE(written > 0);
    REQUIRE(written < size);
    result.resize(Packet::kHeaderBytes + static_cast<usize>(written));
    word(result, 8, static_cast<u32>(result.size()));
    word(result, 12, 1);
    return result;
}
Packet::State emptyState() {
    Packet::State result;
    REQUIRE(result.world.valid());
    REQUIRE(result.fixtures.valid());
    return result;
}
Packet::State populatedState() {
    auto result = emptyState();
    auto& world = result.world;
    world.level = {"TEST", 0, "T0", "State", "LEVELS/T0", "ITEMS/T0", "", 1};
    world.core.catalog[0] = 31;
    world.core.layout = 0x123456789abcdef0ULL;
    world.core.placements.resize(2);
    world.core.placements[0].local[3].x = 7;
    world.core.placements[0].previous[3].x = 6;
    world.core.placements[0].continuity = 19;
    world.core.units.push_back({0, 0.75f, false});
    world.core.moving.push_back({100, Mat4{1}});
    world.core.hidden = {100};
    world.core.contactOnly = {1};
    world.core.blockedFloorExits = {100};
    world.core.animations.push_back({0, 10, 3.25f, true, true, false, false, false});
    world.core.cycles.push_back({0, true});
    world.core.light.points.push_back({{1, 2, 3}, {0.2f, 0.3f, 0.4f}, 5, 2});
    world.core.textureFrame = 23;
    world.core.darken = 0.25f;
    world.destruction.objects.resize(2);
    world.destruction.objects[0].name = "MOVED";
    world.destruction.destroyed = {true, false};
    world.destruction.controlled = {0};
    world.destruction.explosions.push_back({1, 2, 3});
    world.triggers.parents = {-1, -1};
    world.triggers.frameRemainder = 0.5f;
    world.triggers.emptyToggleDelay = 0.125f;
    world.triggers.cameraHeld = true;
    world.triggers.wakes.push_back({3, 4, 5});
    world.textures = {4, 0.5f, 0.5f};
    world.frameRemainder = 0.5f;
    world.textureAdvance = 0.5f;
    world.ambientOffset = 0.125f;
    world.particlesAdvanced = true;
    ParticleDescriptor descriptor;
    descriptor.rate = {1, 1, 1, 1};
    descriptor.particleLife = 30;
    descriptor.emitFrames = 30;
    descriptor.texture = "P_FIRE";
    ParticleEmitter emitter;
    emitter.start(descriptor, Mat4{1}, 17);
    emitter.step(3);
    world.particles.entries.push_back({emitter.checkpoint(), 0});
    world.particleSources.push_back(0);
    world.lighting.ambient = {0.2f, 0.3f, 0.4f};
    world.litNow.color = {0.7f, 0.5f, 0.4f};

    ItemInfo info;
    info.type = ItemInfo::kPowerup;
    info.subtype = ItemInfo::kGold;
    info.name = "COIN";
    world.pickups.definitions.push_back(info);
    ItemInstance placement;
    placement.info = 0;
    placement.name = "GEOMETRY";
    placement.collision.push_back({{0, 1, 0}, {Vec3{0, 0, 0}, Vec3{0, 0, 2}, Vec3{2, 0, 0}}});
    world.pickups.placements.push_back(placement);
    world.pickups.layout = true;
    world.pickups.revealing = true;
    world.pickups.revealTime = 0.25f;
    world.pickups.capturePending = true;

    DestructibleWallsCheckpoint::Wall wall;
    wall.source.info.type = 10;
    wall.source.info.subtype = 42;
    wall.source.info.hitPoints = 10;
    wall.source.instance.info = 0;
    wall.source.instance.collision = placement.collision;
    wall.object = 100;
    wall.health = 7;
    wall.flash = 0.01f;
    wall.bounds.acquisition = DestructibleWallsCheckpoint::Acquisition{{1, 2, 3}, 1, 1, 4, true};
    wall.surface.push_back({{0, 1, 0}, {Vec3{0, 0, 0}, Vec3{0, 0, 2}, Vec3{2, 0, 0}}, 100, 4});
    world.walls.bound = true;
    world.walls.walls.push_back(wall);
    auto& fixtures = result.fixtures;
    fixtures.clouds.push_back({{3, 4, 5}, 7, 0.25f, 9});
    LevelFixturesCheckpoint::Blast blast;
    blast.position = {1, 2, 3};
    blast.radius = 12;
    blast.damage = 31;
    blast.seconds = 1;
    blast.elapsed = 0.25f;
    blast.flags = 0x800;
    blast.started = true;
    blast.players = {3, 0};
    blast.reached = {19, 4};
    blast.opponents = {4, 19};
    blast.playerReady = {{0, 0.2f}, {3, 0.75f}};
    blast.opponentReady = {{19, 0.1f}, {4, 0.2f}};
    fixtures.blasts.push_back(blast);
    fixtures.rubble.resources.push_back({0, std::string{Rubble::kItem}});
    fixtures.rubble.pieces.push_back({0, Mat4{1}});
    fixtures.traps.random = 12345;
    REQUIRE(world.valid());
    REQUIRE(fixtures.valid());
    return result;
}

TEST_CASE("stage packet preserves complete clocks contacts masks particles and pending outputs",
          "[rollback][stage-state-packet]") {
    const auto state = populatedState();
    const auto catalog = Packet::Catalog::build(state);
    REQUIRE(catalog);
    const auto bytes = Packet::encode(state, *catalog);
    REQUIRE(bytes);
    CHECK(bytes->size() <= Packet::kMaxBytes);
    CHECK(Packet::encodedSize(state, *catalog) == bytes->size());
    CHECK(Packet::envelopeValid(*bytes));
    REQUIRE(bytes->size() > Packet::kHeaderBytes);
    CHECK((*bytes)[0] == 'S');
    CHECK((*bytes)[1] == 'T');
    CHECK((*bytes)[2] == 'S');
    CHECK((*bytes)[3] == 'T');
    CHECK((*bytes)[4] == Packet::kVersion);
    CHECK(std::ranges::equal(std::span{*bytes}.subspan(24, 32), catalog->fingerprint()));
    CHECK(Packet::decodedSize(state, *catalog) == word(*bytes, 16));
    const auto decoded = Packet::decode(*bytes, *catalog);
    REQUIRE(decoded);
    CHECK(*decoded == state);
    CHECK(Packet::encode(*decoded, *catalog) == bytes);
    const auto before = *bytes;
    CHECK_FALSE(Packet::decode(*bytes, {}));
    CHECK_FALSE(Packet::encode(state, {}));
    CHECK_FALSE(Packet::encodedSize(state, {}));
    CHECK_FALSE(Packet::decodedSize(state, {}));
    CHECK(*bytes == before);
}

TEST_CASE("stage geometry is catalog backed and same count foreign surfaces are refused",
          "[rollback][stage-state-packet]") {
    const auto original = populatedState();
    const auto catalog = Packet::Catalog::build(original);
    REQUIRE(catalog);
    const auto originalBytes = raw(original, *catalog);
    REQUIRE(originalBytes);
    auto larger = original;
    larger.world.pickups.placements[0].collision.resize(
        1000, original.world.pickups.placements[0].collision[0]);
    larger.world.walls.walls[0].surface.resize(1000, original.world.walls.walls[0].surface[0]);
    const auto largeCatalog = Packet::Catalog::build(larger);
    REQUIRE(largeCatalog);
    const auto largeBytes = raw(larger, *largeCatalog);
    REQUIRE(largeBytes);
    CHECK(largeBytes->size() == originalBytes->size());
    CHECK(Packet::decode(*largeBytes, *largeCatalog) == larger);
    CHECK_FALSE(Packet::decode(*largeBytes, *catalog));
    CHECK_FALSE(Packet::encode(larger, *catalog));
    auto foreign = original;
    foreign.world.pickups.placements[0].collision[0].vertices[1].y = 2;
    const auto foreignCatalog = Packet::Catalog::build(foreign);
    REQUIRE(foreignCatalog);
    CHECK(foreignCatalog->fingerprint() != catalog->fingerprint());
    CHECK_FALSE(Packet::decode(*originalBytes, *foreignCatalog));
    CHECK_FALSE(Packet::encode(foreign, *catalog));
    auto invalidReference = *originalBytes;
    const std::string marker = "GEOMETRY";
    const auto found =
        std::search(invalidReference.begin(), invalidReference.end(), marker.begin(), marker.end());
    REQUIRE(found != invalidReference.end());
    const auto reference =
        static_cast<usize>(found - invalidReference.begin()) + marker.size() + 12 + 12 + 12 * 4;
    word(invalidReference, reference, 0xffffffffU);
    CHECK_FALSE(Packet::decode(invalidReference, *catalog));
    foreign = original;
    foreign.world.core.catalog[7] ^= 1;
    CHECK_FALSE(Packet::encode(foreign, *catalog));
    foreign = original;
    foreign.world.level.name = "FOREIGN";
    CHECK_FALSE(Packet::encode(foreign, *catalog));
}

TEST_CASE("stage packet preserves optional native camera history and validates its boundaries",
          "[rollback][stage-state-packet][stage-camera-packet]") {
    auto state = emptyState();
    const auto catalog = Packet::Catalog::build(state);
    REQUIRE(catalog);
    const auto absent = raw(state, *catalog);
    REQUIRE(absent);
    state.camera.emplace();
    auto& camera = *state.camera;
    camera.level = state.world.level;
    camera.markers = {{7, 2, {1, 2, 3}, {0.2f, 0.3f, 0.4f}}, {8, 1, {4, 5, 6}, {0.5f, 0.6f, 0.7f}}};
    camera.range.radiusMin = 12;
    camera.range.radiusMax = 24;
    camera.range.minPitch = 0.25f;
    camera.range.boundsMin = {-30, -20, -10};
    camera.range.boundsMax = {40, 50, 60};
    camera.camera = {{1, 2, 3}, 0.5f, 0.75f, 0};
    camera.attention = {4, 5, 6};
    camera.distance = 18;
    camera.marker = 0;
    camera.aimedMarker = 1;
    camera.yawRate = 0.001f;
    camera.pitchRate = -0.002f;
    camera.turnStep = 2.5f;
    camera.ringIndex = 3;
    for (usize index = 0; index < camera.ringPositions.size(); ++index) {
        const auto value = static_cast<f32>(index);
        camera.ringPositions[index] = {value, value + 1, value + 2};
        camera.ringDistances[index] = value + 20;
    }
    REQUIRE(camera.valid());
    const auto bytes = raw(state, *catalog);
    REQUIRE(bytes);
    CHECK(Packet::decode(*bytes, *catalog) == state);
    auto invalid = *bytes;
    word(invalid, absent->size() - 8, 2); // Optional camera presence must be canonical.
    CHECK_FALSE(Packet::decode(invalid, *catalog));
    invalid = *bytes;
    word(invalid, absent->size() + 28, TowerCameraCheckpoint::State::kMaxMarkers + 1);
    CHECK_FALSE(Packet::decode(invalid, *catalog));
    invalid = *bytes;
    word(invalid, invalid.size() - 8, TowerCamera::kRing);
    CHECK_FALSE(Packet::decode(invalid, *catalog));
    invalid = *bytes;
    word(invalid, invalid.size() - 12, 0x7fc00000U);
    CHECK_FALSE(Packet::decode(invalid, *catalog));
    auto foreign = state;
    foreign.camera->level.name = "OTHER";
    CHECK_FALSE(Packet::encode(foreign, *catalog));
    foreign = state;
    foreign.camera->markers.resize(TowerCameraCheckpoint::State::kMaxMarkers + 1);
    CHECK_FALSE(Packet::encode(foreign, *catalog));
}

TEST_CASE("stage decoder refuses truncated malformed headers counts booleans and scalars",
          "[rollback][stage-state-packet]") {
    const auto state = emptyState();
    const auto catalog = Packet::Catalog::build(state);
    REQUIRE(catalog);
    const auto encoded = raw(state, *catalog);
    REQUIRE(encoded);
    for (usize length = 0; length < encoded->size(); ++length) {
        auto truncated = *encoded;
        truncated.resize(length);
        if (length >= 12) {
            word(truncated, 8, static_cast<u32>(length));
        }
        CHECK_FALSE(Packet::decode(truncated, *catalog));
    }
    for (const auto offset :
         {usize{0}, usize{4}, usize{8}, usize{12}, usize{16}, usize{20}, usize{24}}) {
        auto invalid = *encoded;
        word(invalid, offset, 0xffffffffU);
        CHECK_FALSE(Packet::decode(invalid, *catalog));
    }
    auto invalid = *encoded;
    word(invalid, Packet::kHeaderBytes, 0xffffffffU); // First string length.
    CHECK_FALSE(Packet::decode(invalid, *catalog));
    invalid = *encoded;
    // Eight empty LevelRef fields, u64 layout, 32 scalar catalog bytes,
    // resource count, then the first dynamic record count.
    constexpr usize kPlacementsCount = Packet::kHeaderBytes + 32 + 8 + 32 * 4 + 4;
    word(invalid, kPlacementsCount, 0xffffffffU);
    CHECK_FALSE(Packet::decode(invalid, *catalog));
    invalid = *encoded;
    word(invalid, invalid.size() - 12, 0x7fc00000U); // Difficulty before absent camera/portals.
    CHECK_FALSE(Packet::decode(invalid, *catalog));
    invalid = *encoded;
    word(invalid, invalid.size() - 12, 0x7f800000U);
    CHECK_FALSE(Packet::decode(invalid, *catalog));
    invalid = *encoded;
    // Empty scenery layout bool followed by two rubble counts, cloud/blast/
    // doomed counts, difficulty and absent camera/portals, each one word.
    word(invalid, invalid.size() - 36, 2);
    CHECK_FALSE(Packet::decode(invalid, *catalog));
    invalid = *encoded;
    invalid.push_back(0);
    rawLengths(invalid);
    CHECK(Packet::envelopeValid(invalid));
    CHECK_FALSE(Packet::decode(invalid, *catalog));
    invalid.resize(Packet::kMaxBytes + 1);
    word(invalid, 8, static_cast<u32>(invalid.size()));
    CHECK_FALSE(Packet::envelopeValid(invalid));
    CHECK_FALSE(Packet::decode(invalid, *catalog));
}

TEST_CASE("stage packet preserves complete portal queues and rejects malformed portal state",
          "[rollback][stage-state-packet][stage-portal-packet]") {
    auto state = emptyState();
    state.portals.emplace();
    auto& portals = *state.portals;
    portals.collision = true;
    ExitPortalsCheckpoint::Portal portal;
    portal.source.info.type = ExitPortals::kExitItem;
    portal.source.info.radius = 2;
    portal.source.instance.info = 0;
    portal.source.instance.minPlayers = 2;
    portal.source.instance.params[4] = 'g';
    portal.source.instance.params[5] = '1';
    portal.source.instance.collision.push_back(
        {{0, 1, 0}, {Vec3{0, 0, 0}, Vec3{0, 0, 2}, Vec3{2, 0, 0}}});
    portal.radius = 2;
    portal.minPlayers = 2;
    portal.tag = "g1";
    portal.destination = LevelRef{"TEST", 0, "G1", "Destination", "LEVELS/G1", "ITEMS", "", 0};
    portal.position = {1, 2, 3};
    portal.transform[3] = {portal.position, 1};
    portal.support = 17;
    portal.supportLocal[3].x = 4;
    portal.alpha = 0.625f;
    portal.departurePosition = Vec3{4, 5, 6};
    portal.action = ExitPortals::kWaiting;
    portal.ticksLeft = 41;
    portal.consumed = true;
    portal.icon.state.transform[3].z = 7;
    portal.icon.state.placement = portal.icon.state.transform;
    portal.icon.state.position.z = 7;
    portals.portals.push_back(portal);
    portals.waiting = {3, 1, 3}; // Repeated native wait messages retain their order.
    portals.visitors = {{3, 6}, {1, 2}};
    REQUIRE(portals.valid());
    const auto catalog = Packet::Catalog::build(state);
    REQUIRE(catalog);
    const auto bytes = raw(state, *catalog);
    REQUIRE(bytes);
    const auto decoded = Packet::decode(*bytes, *catalog);
    REQUIRE(decoded);
    CHECK(*decoded == state);
    CHECK(raw(*decoded, *catalog) == bytes);

    auto absent = state;
    absent.portals.reset();
    const auto without = raw(absent, *catalog);
    REQUIRE(without);
    const usize presence = without->size() - 4;
    auto invalid = *bytes;
    word(invalid, presence, 2);
    CHECK_FALSE(Packet::decode(invalid, *catalog));
    invalid = *bytes;
    word(invalid, presence + 4, ExitPortalsCheckpoint::State::kMaxPortals + 1);
    CHECK_FALSE(Packet::decode(invalid, *catalog));
    invalid = *bytes;
    word(invalid, invalid.size() - 4, 2); // Collision binding boolean.
    CHECK_FALSE(Packet::decode(invalid, *catalog));
    for (const auto change : {0, 1, 2, 3, 4, 5}) {
        auto malformed = state;
        if (change == 0) {
            malformed.portals->visitors.push_back(malformed.portals->visitors[0]);
        } else if (change == 1) {
            malformed.portals->waiting[0] = 4;
        } else if (change == 2) {
            malformed.portals->portals[0].alpha = std::numeric_limits<f32>::quiet_NaN();
        } else if (change == 3) {
            malformed.portals->portals[0].archive = 2;
        } else if (change == 4) {
            malformed.portals->portals[0].source.instance.collision[0].vertices[0].x = 1;
        } else {
            malformed.portals->sequences[0] = 0; // No trusted ordinary tree.
        }
        CHECK_FALSE(Packet::encode(malformed, *catalog));
    }
}

TEST_CASE("stage corruption never bypasses canonical field or catalog validation",
          "[rollback][stage-state-packet]") {
    const auto state = populatedState();
    const auto catalog = Packet::Catalog::build(state);
    REQUIRE(catalog);
    const auto encoded = raw(state, *catalog);
    REQUIRE(encoded);
    usize refused = 0;
    for (usize offset = Packet::kHeaderBytes; offset < encoded->size(); ++offset) {
        auto altered = *encoded;
        altered[offset] ^= 0x80;
        const auto decoded = Packet::decode(altered, *catalog);
        if (!decoded) {
            ++refused;
            continue;
        }
        CHECK(decoded->world.valid());
        CHECK(decoded->fixtures.valid());
        CHECK(raw(*decoded, *catalog) == altered);
    }
    CHECK(refused > 100);
    auto invalid = state;
    invalid.world.particles.entries[0].emitter.phase = static_cast<ParticleEmitter::Phase>(255);
    CHECK_FALSE(Packet::encode(invalid, *catalog));
    invalid = state;
    invalid.world.core.placements[0].local[1].x = std::numeric_limits<f32>::infinity();
    CHECK_FALSE(Packet::encode(invalid, *catalog));
}

TEST_CASE("lossless stage compression retains every owner beyond the raw wire budget",
          "[rollback][stage-state-packet]") {
    auto state = emptyState();
    state.world.core.placements.resize(4000);
    state.world.destruction.objects.resize(4000);
    state.world.destruction.destroyed.resize(4000);
    state.world.triggers.parents.resize(4000, -1);
    // Repeated but genuinely nonidentity transforms exceed the raw budget.
    for (auto& placement : state.world.core.placements) {
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 3; ++row) {
                placement.local[column][row] = static_cast<f32>(column * 3 + row + 2);
            }
        }
        placement.previous = placement.local;
    }
    REQUIRE(state.world.valid());
    const auto catalog = Packet::Catalog::build(state);
    REQUIRE(catalog);
    const auto size = Packet::decodedSize(state, *catalog);
    REQUIRE(size);
    CHECK(*size > Packet::kMaxBytes);
    CHECK_FALSE(raw(state, *catalog));
    const auto bytes = Packet::encode(state, *catalog);
    REQUIRE(bytes);
    CHECK(word(*bytes, 12) == 1);
    CHECK(bytes->size() < Packet::kMaxBytes);
    CHECK(Packet::encodedSize(state, *catalog) == bytes->size());
    CHECK(Packet::decode(*bytes, *catalog) == state);
    CHECK(state.world.core.placements.size() == 4000);
    auto invalid = emptyState();
    invalid.world.core.placements.resize(WorldStateCheckpoint::State::kMaxObjects + 1);
    CHECK_FALSE(Packet::Catalog::build(invalid));
}

TEST_CASE("stage compression rejects bombs invalid codecs lengths and trailing compressed bytes",
          "[rollback][stage-state-packet][stage-compression]") {
    const auto state = populatedState();
    const auto catalog = Packet::Catalog::build(state);
    REQUIRE(catalog);
    const auto original = raw(state, *catalog);
    REQUIRE(original);
    const auto encoded = Packet::encode(state, *catalog);
    REQUIRE(encoded);
    REQUIRE(word(*encoded, 12) == 1);
    REQUIRE(encoded->size() < original->size());
    CHECK(Packet::decode(*encoded, *catalog) == state);
    CHECK(Packet::decodedSize(state, *catalog) == original->size() - Packet::kHeaderBytes);
    CHECK(Packet::encodedSize(state, *catalog) == encoded->size());

    for (usize length = 0; length < encoded->size(); ++length) {
        auto invalid = *encoded;
        invalid.resize(length);
        if (length >= 12) {
            word(invalid, 8, static_cast<u32>(length));
        }
        CHECK_FALSE(Packet::decode(invalid, *catalog));
    }
    for (const auto [offset, value] :
         {std::pair{usize{0}, u32{0}}, std::pair{usize{4}, Packet::kVersion - 1},
          std::pair{usize{8}, u32{0}}, std::pair{usize{12}, u32{2}},
          std::pair{usize{12}, u32{0xffffffffU}}, std::pair{usize{16}, u32{0}},
          std::pair{usize{16}, static_cast<u32>(Packet::kMaxDecodedBytes + 1)},
          std::pair{usize{16}, u32{0xffffffffU}}, std::pair{usize{20}, u32{1}}}) {
        auto invalid = *encoded;
        word(invalid, offset, value);
        CHECK_FALSE(Packet::envelopeValid(invalid));
        CHECK_FALSE(Packet::decode(invalid, *catalog));
    }
    for (const s32 delta : {-1, 1}) {
        auto invalid = *encoded;
        word(invalid, 16, static_cast<u32>(static_cast<s32>(word(invalid, 16)) + delta));
        REQUIRE(Packet::envelopeValid(invalid));
        CHECK_FALSE(Packet::decode(invalid, *catalog));
    }
    for (const auto suffix : {std::vector<u8>{0}, std::vector<u8>{0, 0},
                              std::vector<u8>{0xff, 0xff, 0xff, 0xff}, std::vector<u8>{0x10, 0}}) {
        auto invalid = *encoded;
        invalid.insert(invalid.end(), suffix.begin(), suffix.end());
        word(invalid, 8, static_cast<u32>(invalid.size()));
        REQUIRE(Packet::envelopeValid(invalid));
        CHECK_FALSE(Packet::decode(invalid, *catalog));
    }
    auto invalid = *encoded;
    // A known-valid catalog cannot authorize allocation above either budget.
    invalid.resize(Packet::kMaxBytes + 1);
    word(invalid, 8, static_cast<u32>(invalid.size()));
    CHECK_FALSE(Packet::envelopeValid(invalid));
    CHECK_FALSE(Packet::decode(invalid, *catalog));
    invalid = *encoded;
    invalid[24] ^= 1;
    REQUIRE(Packet::envelopeValid(invalid));
    CHECK_FALSE(Packet::decode(invalid, *catalog));
    invalid = *encoded;
    std::fill(invalid.begin() + static_cast<std::ptrdiff_t>(Packet::kHeaderBytes), invalid.end(),
              0);
    CHECK_FALSE(Packet::decode(invalid, *catalog));

    auto hostile = *original;
    word(hostile, Packet::kHeaderBytes, static_cast<u32>(Packet::kMaxString + 1));
    CHECK_FALSE(Packet::decode(pack(hostile), *catalog));
    hostile = *original;
    word(hostile, hostile.size() - 12, 0x7fc00000U);
    CHECK_FALSE(Packet::decode(pack(hostile), *catalog));
    hostile = *original;
    hostile.push_back(0);
    rawLengths(hostile);
    CHECK_FALSE(Packet::decode(pack(hostile), *catalog));
    const auto empty = emptyState();
    const auto emptyCatalog = Packet::Catalog::build(empty);
    REQUIRE(emptyCatalog);
    const auto emptyBytes = raw(empty, *emptyCatalog);
    REQUIRE(emptyBytes);
    hostile = *emptyBytes;
    constexpr usize kPlacementsCount = Packet::kHeaderBytes + 32 + 8 + 32 * 4 + 4;
    word(hostile, kPlacementsCount, static_cast<u32>(Packet::kMaxRecords + 1));
    CHECK_FALSE(Packet::decode(pack(hostile), *emptyCatalog));
    CHECK_FALSE(Packet::encode(state, *catalog, static_cast<Packet::Encoding>(255)));
}

TEST_CASE("small stage bodies remain raw and incompressible complete states retain the wire cap",
          "[rollback][stage-state-packet][stage-compression]") {
    auto state = emptyState();
    auto catalog = Packet::Catalog::build(state);
    REQUIRE(catalog);
    const auto small = Packet::encode(state, *catalog);
    REQUIRE(small);
    REQUIRE(small->size() - Packet::kHeaderBytes < 1024);
    CHECK(word(*small, 12) == 0);
    CHECK(small == raw(state, *catalog));
    CHECK(Packet::decode(*small, *catalog) == state);
    auto invalid = *small;
    word(invalid, 16, word(invalid, 16) + 1);
    CHECK_FALSE(Packet::envelopeValid(invalid));
    CHECK_FALSE(Packet::decode(invalid, *catalog));

    constexpr usize kCount = 12000;
    state.world.core.placements.resize(kCount);
    state.world.destruction.objects.resize(kCount);
    state.world.destruction.destroyed.resize(kCount);
    state.world.triggers.parents.resize(kCount, -1);
    u32 random = 0x12345678U;
    const auto scalar = [&]() {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        return std::bit_cast<f32>(0x3f800000U | (random & 0x007fffffU));
    };
    for (auto& placement : state.world.core.placements) {
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 3; ++row) {
                placement.local[column][row] = scalar();
                placement.previous[column][row] = scalar();
            }
        }
    }
    REQUIRE(state.world.valid());
    catalog = Packet::Catalog::build(state);
    REQUIRE(catalog);
    const auto size = Packet::decodedSize(state, *catalog);
    REQUIRE(size);
    CHECK(*size > Packet::kMaxBytes);
    CHECK(*size <= Packet::kMaxDecodedBytes);
    CHECK_FALSE(raw(state, *catalog));
    CHECK_FALSE(Packet::encode(state, *catalog));
    CHECK_FALSE(Packet::encodedSize(state, *catalog));
    CHECK(state.world.core.placements.size() == kCount);
}

TEST_CASE("sparse stage matrices are lossless canonical and independent of catalog pose",
          "[rollback][stage-state-packet][stage-sparse-matrix]") {
    auto state = emptyState();
    state.world.core.placements.resize(1);
    state.world.destruction.objects.resize(1);
    state.world.destruction.destroyed.resize(1);
    state.world.triggers.parents.push_back(-1);
    const auto catalog = Packet::Catalog::build(state);
    REQUIRE(catalog);
    const auto identity = raw(state, *catalog);
    REQUIRE(identity);
    constexpr usize kFirstMatrix = Packet::kHeaderBytes + 32 + 8 + 32 * 4 + 4 + 4;
    REQUIRE(kFirstMatrix + 12 <= identity->size());
    CHECK((*identity)[kFirstMatrix] == 0);
    CHECK((*identity)[kFirstMatrix + 4] == 0); // Previous identity mask, not 16 floats.
    CHECK(Packet::decode(*identity, *catalog) == state);

    const auto variant = GENERATE(0, 1, 2, 3);
    auto& matrix = state.world.core.placements[0].local;
    usize values = 0;
    if (variant == 0) {
        matrix[3] = {1.25f, 2.5f, -3.75f, 1};
        values = 3;
    } else if (variant == 1) {
        matrix[0].y = -0.0f;
        matrix[2].w = -0.0f;
        values = 2;
    } else if (variant == 2) {
        matrix[1].z = std::bit_cast<f32>(u32{1}); // Smallest positive subnormal.
        matrix[2].y = std::bit_cast<f32>(u32{0x80000001U});
        matrix[0].x = -1;
        values = 3;
    } else {
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 3; ++row) {
                matrix[column][row] = static_cast<f32>(column * 3 + row + 2);
            }
        }
        matrix[0].w = matrix[1].w = matrix[2].w = -0.0f;
        values = 15;
    }
    REQUIRE(state.world.valid());
    const auto bytes = raw(state, *catalog);
    REQUIRE(bytes);
    CHECK(bytes->size() == identity->size() + values * 4);
    CHECK(Packet::encodedSize(state, *catalog, Packet::Encoding::Raw) == bytes->size());
    const auto decoded = Packet::decode(*bytes, *catalog);
    REQUIRE(decoded);
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            CHECK(std::bit_cast<u32>(decoded->world.core.placements[0].local[column][row]) ==
                  std::bit_cast<u32>(matrix[column][row]));
            CHECK(std::bit_cast<u32>(decoded->world.core.placements[0].previous[column][row]) ==
                  std::bit_cast<u32>(state.world.core.placements[0].previous[column][row]));
        }
    }
    CHECK(raw(*decoded, *catalog) == bytes);
    // Compression must preserve the same signed-zero and subnormal bits, not
    // merely floating-point equality of the resulting matrices.
    const auto compressed = Packet::encode(state, *catalog);
    REQUIRE(compressed);
    const auto compressedState = Packet::decode(*compressed, *catalog);
    REQUIRE(compressedState);
    CHECK(raw(*compressedState, *catalog) == bytes);
    auto invalid = *identity;
    word(invalid, kFirstMatrix, 0x10000U);
    CHECK_FALSE(Packet::decode(invalid, *catalog));
    invalid = *identity;
    word(invalid, 4, Packet::kVersion - 1);
    CHECK_FALSE(Packet::envelopeValid(invalid));
    CHECK_FALSE(Packet::decode(invalid, *catalog));

    for (const auto [mask, bits] :
         {std::pair{u32{1}, u32{0x3f800000U}}, // Redundant diagonal +1.
          std::pair{u32{2}, u32{0}},           // Redundant off-diagonal +0.
          std::pair{u32{2}, u32{0x7fc00000U}}, std::pair{u32{2}, u32{0x7f800000U}}}) {
        invalid = *identity;
        invalid.insert(invalid.begin() + static_cast<std::ptrdiff_t>(kFirstMatrix + 4), 4, 0);
        word(invalid, kFirstMatrix, mask);
        word(invalid, kFirstMatrix + 4, bits);
        rawLengths(invalid);
        CHECK(Packet::envelopeValid(invalid));
        CHECK_FALSE(Packet::decode(invalid, *catalog));
    }
    for (usize extra = 0; extra < 4; ++extra) {
        invalid = *identity;
        invalid.resize(kFirstMatrix + 4 + extra);
        word(invalid, kFirstMatrix, 2);
        rawLengths(invalid);
        CHECK_FALSE(Packet::decode(invalid, *catalog));
    }
}

TEST_CASE("opaque stage supplement needs its trusted catalog after combat transport",
          "[rollback][stage-state-packet]") {
    const auto stage = populatedState();
    const auto catalog = Packet::Catalog::build(stage);
    REQUIRE(catalog);
    CombatStatePacket::State state;
    state.combat.health = Mt19937{17}.checkpoint();
    state.stage = Packet::encode(stage, *catalog);
    REQUIRE(state.stage);
    REQUIRE(state.valid());
    const auto encoded = CombatStatePacket::encode(state);
    REQUIRE(encoded);
    const auto decoded = CombatStatePacket::decode(*encoded);
    REQUIRE(decoded);
    REQUIRE(decoded->stage);
    CHECK(*decoded == state);
    CHECK_FALSE(Packet::decode(*decoded->stage, {}));
    CHECK(Packet::decode(*decoded->stage, *catalog) == stage);
    auto altered = *decoded->stage;
    altered[24] ^= 1;
    CHECK(Packet::envelopeValid(altered));
    CHECK_FALSE(Packet::decode(altered, *catalog));
}

struct NativeStage {
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelFixtures fixtures;
    LevelWorldCheckpoint worldOwner{world};
    LevelFixturesCheckpoint fixtureOwner{fixtures};
    LevelCatalog levels;
    ExitPortals portals;
    std::unique_ptr<ExitPortalsCheckpoint> portalOwner;
    NativeStage(const std::filesystem::path& root, const LevelRef& level, bool native) {
        REQUIRE(world.load(device, root, level));
        world.setPlayerCount(1);
        if (native) {
            REQUIRE(weapons.load(root / "WEAPONS"));
        }
        fixtures.bind({device, world, weapons, effects, audio, 1});
        fixtures.setPlayerCount(1);
        REQUIRE(worldOwner.bindResources(device));
        REQUIRE(fixtureOwner.bindResources());
        if (native) {
            REQUIRE(levels.load(root));
            portals.bind(device, world.layout(), world.items(), levels, &world.collision(),
                         &world.realmItems());
            portalOwner = std::make_unique<ExitPortalsCheckpoint>(
                portals, world.layout(), world.items(), &world.realmItems());
            REQUIRE(portalOwner->bindResources());
        }
    }
    Packet::State state() const {
        const auto worldState = worldOwner.checkpoint();
        const auto fixtureState = fixtureOwner.checkpoint();
        REQUIRE(worldState);
        REQUIRE(fixtureState);
        Packet::State result{*worldState, *fixtureState};
        if (portalOwner) {
            result.portals = portalOwner->checkpoint();
            REQUIRE(result.portals);
        }
        return result;
    }
    void step(f32 seconds) {
        world.capturePresentation();
        world.update(seconds);
        world.updateTriggers(seconds, {});
        if (portalOwner) {
            portals.animate(seconds);
        }
    }
};

TEST_CASE("stage packet restores independently bound native motion and particle continuation",
          "[rollback][stage-state-packet]") {
    const auto rate = GENERATE(30, 60);
    const auto root = test::sampleLevel("stage-packet-native-continuation");
    const LevelRef level{"TEST", 0, "TEST", "", "", "", "", 0};
    NativeStage first(root, level, false), second(root, level, false);
    const auto catalog = Packet::Catalog::build(first.state());
    const auto otherCatalog = Packet::Catalog::build(second.state());
    REQUIRE(catalog);
    REQUIRE(otherCatalog);
    CHECK(catalog->fingerprint() == otherCatalog->fingerprint());
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    for (s32 tick = 0; tick < 13; ++tick) {
        first.step(seconds);
    }
    const auto current = first.state();
    const auto bytes = Packet::encode(current, *catalog);
    REQUIRE(bytes);
    const auto restored = Packet::decode(*bytes, *otherCatalog);
    REQUIRE(restored);
    const auto uploads = second.device.textureUpdates;
    const auto textures = second.device.texturesCreated;
    auto world = second.worldOwner.prepareRestore(restored->world);
    auto fixtures = second.fixtureOwner.prepareRestore(restored->fixtures);
    REQUIRE(world);
    REQUIRE(fixtures);
    REQUIRE(second.worldOwner.canCommit(*world));
    REQUIRE(second.fixtureOwner.canCommit(*fixtures));
    REQUIRE(second.worldOwner.commitRestore(std::move(*world)));
    REQUIRE(second.fixtureOwner.commitRestore(std::move(*fixtures)));
    CHECK(second.state() == current);
    for (s32 tick = 0; tick < 20; ++tick) {
        first.step(seconds);
        second.step(seconds);
        CHECK(first.state() == second.state());
    }
    CHECK(second.device.textureUpdates == uploads);
    CHECK(second.device.texturesCreated == textures);
}

TEST_CASE("native stage wire reports honest G1 Tower and L1 size within the fixed budget",
          "[rollback][stage-state-packet][stage-packet-size][assets]") {
    const std::string name = GENERATE(std::string{"G1"}, std::string{"TOWER"}, std::string{"L1"});
    const auto root = test::assetOrSkip("WDATA/SECRET.WAD").parent_path().parent_path();
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    const auto level = name == "TOWER" ? std::optional{LevelRef::tower()} : levels.byName(name);
    REQUIRE(level);
    NativeStage native(root, *level, true);
    const auto initial = native.state();
    const auto catalog = Packet::Catalog::build(initial);
    REQUIRE(catalog);
    for (s32 tick = 0; tick < 12; ++tick) {
        native.step(1.0f / 60);
    }
    const auto state = native.state();
    const auto decodedSize = Packet::decodedSize(state, *catalog);
    REQUIRE(decodedSize);
    const auto begin = std::chrono::steady_clock::now();
    const auto bytes = Packet::encode(state, *catalog);
    const f64 milliseconds =
        std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - begin).count();
    REQUIRE(bytes);
    WARN("stage-wire " << name << " wire_bytes=" << bytes->size()
                       << " decoded_body_bytes=" << *decodedSize << " budget=" << Packet::kMaxBytes
                       << " encode_ms=" << milliseconds
                       << " placements=" << state.world.core.placements.size());
    CHECK(bytes->size() <= Packet::kMaxBytes);
    CHECK(Packet::encodedSize(state, *catalog) == bytes->size());
    CHECK(Packet::decode(*bytes, *catalog) == state);
}
} // namespace
