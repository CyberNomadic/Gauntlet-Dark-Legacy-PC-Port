#include <bit>
#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/Sha256.h"

#include "game/netplay/CombatStatePacket.h"
#include "game/netplay/EnemyMissilesCheckpoint.h"
#include "game/netplay/EnemyPoolCheckpoint.h"
#include "game/netplay/GeneratorCheckpoint.h"
#include "game/netplay/PlacedEnemyCheckpoint.h"
#include "game/screens/CombatAgreement.h"
#include "game/screens/OpponentPredictionFixture.h"

namespace {
using namespace gdl;
using namespace gdl::game;

auto digest(std::span<const u8> bytes) {
    Sha256 hash;
    hash.update(bytes);
    return hash.finish();
}
template <class Codec, class State> void sameCanonicalBytes(const State& state) {
    const auto bytes = Codec::encode(state);
    REQUIRE(bytes);
    const auto direct = Codec::canonicalDigest(state);
    REQUIRE(direct);
    CHECK(*direct == digest(*bytes));
}
} // namespace

TEST_CASE("agreement opponent sinks hash unchanged native canonical bytes",
          "[rollback][combat-agreement][assets]") {
    auto native = std::make_unique<test::ContinuationFixture>();
    native->spawn();
    const auto state = std::make_unique<OpponentState>(native->capture());
    sameCanonicalBytes<GeneratorCheckpoint>(state->generators);
    sameCanonicalBytes<EnemyPoolCheckpoint>(state->enemies);
    sameCanonicalBytes<EnemyMissilesCheckpoint>(state->missiles);
    PlacedEnemies placed;
    placed.bind({});
    const auto placements = placed.checkpoint();
    REQUIRE(placements);
    sameCanonicalBytes<PlacedEnemyCheckpoint>(*placements);

    const auto before = EnemyPoolCheckpoint::canonicalDigest(state->enemies);
    ++state->enemies.frame;
    CHECK(EnemyPoolCheckpoint::canonicalDigest(state->enemies) != before);
}

TEST_CASE("agreement combat and stage visitors hash uncompressed explicit fields",
          "[rollback][combat-agreement]") {
    auto combat = std::make_unique<CombatStatePacket::State>();
    combat->combat.health = Mt19937{73}.checkpoint();
    REQUIRE(combat->valid());
    const auto encoded = CombatStatePacket::encode(*combat);
    REQUIRE(encoded);
    const auto direct = CombatStatePacket::canonicalDigest(combat->combat, combat->adapter);
    REQUIRE(direct);
    CHECK(*direct ==
          digest(std::span<const u8>{*encoded}.subspan(CombatStatePacket::kHeaderBytes)));

    OrdinaryCombatCheckpoint::Stage stage;
    stage.world.core.placements.resize(1);
    stage.world.core.placements[0].local[3].x = 4;
    stage.world.core.placements[0].previous[3].x = 3;
    stage.world.destruction.objects.resize(1);
    stage.world.destruction.destroyed.resize(1);
    stage.world.triggers.parents = {-1};
    REQUIRE(stage.world.valid());
    const auto catalog = StageStatePacket::Catalog::build(stage);
    REQUIRE(catalog);
    const auto raw = StageStatePacket::encode(stage, *catalog, StageStatePacket::Encoding::Raw);
    REQUIRE(raw);
    const auto stageDigest = StageStatePacket::canonicalDigest(stage, *catalog);
    REQUIRE(stageDigest);
    CHECK(*stageDigest ==
          digest(std::span<const u8>{*raw}.subspan(StageStatePacket::kHeaderBytes)));
    const auto normalized = StageStatePacket::canonicalDigest(stage, *catalog, true);
    REQUIRE(normalized);
    stage.world.core.light.points.push_back({{1, 2, 3}, {1, 0, 0}, 5, 2});
    stage.world.lighting.points = stage.world.core.light.points;
    stage.world.litNow.points = stage.world.core.light.points;
    REQUIRE(stage.world.valid());
    CHECK(StageStatePacket::canonicalDigest(stage, *catalog, true) == normalized);
    CHECK(StageStatePacket::canonicalDigest(stage, *catalog) != stageDigest);
    stage.world.core.placements[0].previous[3].x = 2;
    CHECK(StageStatePacket::canonicalDigest(stage, *catalog, true) != normalized);
    stage.world.core.placements[0].previous[3].x = 3;
    stage.world.core.placements[0].local[1].x = -0.0f;
    CHECK(StageStatePacket::canonicalDigest(stage, *catalog, true) != normalized);

    // A digest is deliberately not a validation certificate. Mutating an
    // omitted cache may match; public wire/native preparation must still refuse.
    stage.world.core.light.points[0].radius = -1;
    CHECK_FALSE(stage.world.valid());
    CHECK_FALSE(StageStatePacket::encode(stage, *catalog));
}

TEST_CASE("agreement context retains effective simulation inputs and clock bits",
          "[rollback][combat-agreement]") {
    OrdinaryCombatTick::Frame frame;
    const auto heldCamera = CombatAgreement::context(frame, false);
    const auto nativeCamera = CombatAgreement::context(frame, true);
    REQUIRE(heldCamera);
    REQUIRE(nativeCamera);
    frame.camera.position.x = 13;
    frame.attention.y = 7;
    CHECK(CombatAgreement::context(frame, false) != heldCamera);
    CHECK(CombatAgreement::context(frame, true) == nativeCamera);
    frame.damage.playSeconds = 1;
    CHECK(CombatAgreement::context(frame, true) != nativeCamera);
    frame.damage.playSeconds = 0;
    frame.autoMelee = true;
    CHECK(CombatAgreement::context(frame, true) != nativeCamera);
    frame.autoMelee = false;
    frame.damage.playSeconds = -0.0f;
    CHECK(CombatAgreement::context(frame, true) != nativeCamera);
}

TEST_CASE("agreement normalizes only draw-overwritten item poses not simulation poses",
          "[rollback][combat-agreement]") {
    OrdinaryCombatCheckpoint::Stage stage;
    TreePose::Checkpoint pose{"TREE", {{"ROOT", -1, {0, 0, 0}}}, {NodePose{}}};
    REQUIRE(pose.valid());
    auto& scenery = stage.fixtures.scenery;
    scenery.layout = true;
    StaticSceneryCheckpoint::Prop prop;
    prop.instance = 0;
    prop.definition.type = 10;
    prop.definition.name = "TREE";
    prop.placement.info = 0;
    prop.figure.bound = prop.figure.archive = true;
    prop.figure.state.pose = pose;
    prop.figure.state.presentationPose = pose;
    scenery.props.push_back(prop);
    REQUIRE(scenery.valid());
    auto& pickups = stage.world.pickups;
    pickups.layout = true;
    pickups.archives = {true};
    pickups.textures.resize(1);
    pickups.figures = {{"OLD_TREE", 0}, {"TREE", 0}};
    ItemInfo info;
    info.type = ItemInfo::kPowerup;
    info.name = "TREE";
    pickups.definitions.push_back(info);
    PlacedItemsCheckpoint::Item item;
    item.info = 0;
    item.resource = 1;
    item.pose = pose;
    item.presentationPose = PlacedItemsCheckpoint::Pose{0, pose};
    item.presentationPose->pose.tree = "OLD_TREE";
    pickups.items.push_back(item);
    REQUIRE(pickups.valid());
    const auto catalog = StageStatePacket::Catalog::build(stage);
    REQUIRE(catalog);
    const auto original = StageStatePacket::canonicalDigest(stage, *catalog);
    const auto normalized = StageStatePacket::canonicalDigest(stage, *catalog, true);
    REQUIRE(original);
    REQUIRE(normalized);
    scenery.props[0].figure.state.presentationPose->poses[0].position.x = 3;
    pickups.items[0].presentationPose->pose.poses[0].position.y = 2;
    REQUIRE(scenery.valid());
    REQUIRE(pickups.valid());
    CHECK(StageStatePacket::canonicalDigest(stage, *catalog) != original);
    CHECK(StageStatePacket::canonicalDigest(stage, *catalog, true) == normalized);
    pickups.items[0].pose.poses[0].position.z = 4;
    CHECK(StageStatePacket::canonicalDigest(stage, *catalog, true) != normalized);
    pickups.items[0].pose.poses[0].position.z = 0;
    scenery.props[0].figure.state.presentationPose->nodes[0].parent = 5;
    CHECK_FALSE(scenery.valid());
    CHECK_FALSE(StageStatePacket::encode(stage, *catalog));
}
