#include <array>
#include <limits>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "../../engine/world/SampleLevel.h"
#include "FakeRenderDevice.h"
#include "game/world/LevelCatalog.h"
#include "game/world/LevelTriggers.h"
#include "game/world/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    WorldScene scene;
    WorldAnimator animator;
    WorldCollision collision;
    LevelTriggers triggers;

    explicit Fixture(std::string_view name) {
        const auto directory = test::sampleLevel(name);
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"WALL","position":[0,0,0],"flags":4096},
                     {"name":"WALL","position":[20,0,0],"flags":4096},
                     {"name":"WALL","position":[40,0,0],"flags":4096},
                     {"name":"WALL","position":[60,0,0],"flags":4096}],
          "itemInfos":[{"type":5,"subtype":24,"name":"BRIDGEPAD","radius":2,"height":5},
                       {"type":5,"subtype":31,"name":"BRIDGEPAD","radius":2,"height":5}],
          "itemInstances":[
             {"info":0,"position":[0,0,0],"params":[0,0,80,0,4,2,1,0,0,0,0,0]},
             {"info":0,"position":[20,0,0],"params":[1,0,4,48,4,3,8,9,0,0,100,0]},
             {"info":1,"flags":2,"position":[40,0,0],"params":[2,0,2,0,4,255,9,0,0,0,50,0]},
             {"info":0,"position":[60,0,0],"params":[3,0,0,4,4,4,7,0,0,0,50,0]}]})");
        writeTextFile(directory / "animations.json", R"({"trees":[{"name":"BRIDGEPAD",
          "nodes":[{"name":"WALL","object":"WALL","parent":-1,"position":[0,0,0]}],
          "sequences":[{"name":"OFF","frames":10,"rate":30},
                       {"name":"DOWN","frames":4,"rate":30},
                       {"name":"ON","frames":10,"rate":30},
                       {"name":"UP","frames":4,"rate":30}]}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(items.load(directory));
        REQUIRE(scene.build(layout, items.models, items.textures, device));
        animator.bind(layout);
        triggers.bind(layout, animator, &collision);
        triggers.bindFigures(device, layout, items);
        triggers.setPlayerCount(2);
        REQUIRE(triggers.size() == 4);
        REQUIRE(triggers.figure(0));
        REQUIRE_FALSE(triggers.figure(2));
    }
    LevelTriggers::Checkpoint state() const {
        const auto value = triggers.checkpoint();
        REQUIRE(value);
        return *value;
    }
    void step(f32 seconds, std::span<const TriggerVisitor> visitors) {
        animator.step(seconds, scene);
        triggers.update(seconds, visitors, animator, scene, &collision);
    }
};

TEST_CASE("complete trigger state resumes native contacts queues and figure clocks",
          "[triggers-checkpoint]") {
    Fixture fixture("complete-trigger-phase");
    const f32 seconds = GENERATE(1.0f / 60, 1.0f / 30);
    const auto uploads = fixture.device.texturesCreated;
    bool refusals = false;
    bool wakes = false;
    bool lessons = false;
    bool settled = false;
    bool held = false;
    for (s32 tick = 0; tick < 240; ++tick) {
        std::array<TriggerVisitor, 2> visitors{};
        visitors[0].party = 3;
        visitors[1].party = 1;
        visitors[0].height = visitors[1].height = 5;
        const s32 phase = (tick / 20) % 5;
        visitors[0].position = {static_cast<f32>(phase * 20), 0, 0};
        visitors[1].position = {phase == 3 && tick % 20 >= 10 ? 60.0f : 100.0f, 0, 0};
        visitors[0].crystals[1] = tick >= 100 ? 15 : 0;
        fixture.triggers.setCameraHeld(tick % 12 >= 6);
        if (tick % 17 == 0) {
            fixture.triggers.shoot(2);
        }
        const auto before = fixture.state();
        const auto animator = fixture.animator;
        const auto geometry = fixture.scene.geometry();
        const auto collision = fixture.collision;
        auto rewind = fixture.triggers.prepareRestore(before);
        REQUIRE(rewind);
        fixture.step(seconds, visitors);
        const auto expected = fixture.state();
        refusals |= !expected.refusals.empty();
        wakes |= !expected.wakes.empty();
        lessons |= !expected.lessons.empty();
        settled |= !expected.settled.empty();
        held |= expected.triggers[3].heldContacts == 10;
        const auto expectedAnimation = fixture.animator.checkpoint();
        std::vector<Mat4> transforms;
        for (usize object = 0; object < fixture.layout.objects().size(); ++object) {
            transforms.push_back(fixture.scene.worldTransform(object));
        }
        REQUIRE(fixture.triggers.canCommit(*rewind));
        REQUIRE(fixture.triggers.commitRestore(std::move(*rewind)));
        CHECK(fixture.state() == before);
        // These are external owners, deliberately restored separately. A trigger
        // restore itself must not fire its commands or pose world geometry.
        CHECK(fixture.animator.checkpoint() == expectedAnimation);
        fixture.animator = animator;
        fixture.collision = collision;
        REQUIRE(fixture.scene.applyGeometry(geometry));
        fixture.step(seconds, visitors);
        CHECK(fixture.state() == expected);
        CHECK(fixture.animator.checkpoint() == expectedAnimation);
        for (usize object = 0; object < transforms.size(); ++object) {
            CHECK(fixture.scene.worldTransform(object) == transforms[object]);
        }
    }
    CHECK(refusals);
    CHECK(wakes);
    CHECK(lessons);
    CHECK(settled);
    CHECK(held);
    CHECK(fixture.device.texturesCreated == uploads);
    const auto pending = fixture.state();
    CHECK(fixture.triggers.takeRefusals() == pending.refusals);
    CHECK(fixture.triggers.takeLessons() == pending.lessons);
    CHECK(fixture.triggers.takeOpenings() == pending.openings);
    CHECK(fixture.triggers.takeSettled() == pending.settled);
    CHECK(fixture.triggers.takeCameraCues() == pending.cameraCues);
    CHECK(fixture.triggers.takeWakes() == pending.wakes);
    REQUIRE(fixture.triggers.restore(pending));
    CHECK(fixture.state() == pending);
}

TEST_CASE("trigger plans bind trusted owner roster resources and sibling generation",
          "[triggers-checkpoint]") {
    Fixture host("trigger-restore-host");
    Fixture guest("trigger-restore-guest");
    const std::array visitors{TriggerVisitor{.position = {20, 0, 0}, .party = 2}};
    host.step(1.0f / 60, visitors);
    const auto state = host.state();
    const auto uploads = guest.device.texturesCreated;
    REQUIRE(guest.triggers.restore(state));
    CHECK(guest.state() == state);
    CHECK(guest.device.texturesCreated == uploads);
    auto first = host.triggers.prepareRestore(state);
    auto sibling = host.triggers.prepareRestore(state);
    REQUIRE(first);
    REQUIRE(sibling);
    CHECK_FALSE(guest.triggers.commitRestore(std::move(*first)));
    REQUIRE(host.triggers.canCommit(*first));
    REQUIRE(host.triggers.commitRestore(std::move(*first)));
    CHECK_FALSE(host.triggers.canCommit(*sibling));
    first = host.triggers.prepareRestore(state);
    REQUIRE(first);
    REQUIRE(host.triggers.restoreMotion(host.triggers.motion()));
    CHECK_FALSE(host.triggers.canCommit(*first));
    first = host.triggers.prepareRestore(host.state());
    REQUIRE(first);
    host.triggers.bindFigures(host.device, host.layout, host.items);
    CHECK_FALSE(host.triggers.canCommit(*first));
    first = host.triggers.prepareRestore(host.state());
    REQUIRE(first);
    host.triggers.clear();
    CHECK_FALSE(host.triggers.commitRestore(std::move(*first)));
}

TEST_CASE("complete trigger state refuses malformed and foreign records atomically",
          "[triggers-checkpoint]") {
    Fixture fixture("trigger-invalid-state");
    const auto before = fixture.state();
    auto invalid = before;
    const s32 variant = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13);
    switch (variant) {
    case 0: invalid.triggers[0].spot.x = std::numeric_limits<f32>::quiet_NaN(); break;
    case 1: invalid.triggers[0].heldContacts = 16; break;
    case 2: invalid.triggers[0].next = 999; break;
    case 3: invalid.targets[0].height = 1e9f; break;
    case 4: invalid.targets[0].subtree.push_back(999); break;
    case 5: invalid.figures[0]->animation.frame = std::numeric_limits<f32>::infinity(); break;
    case 6: invalid.refusals.push_back({999, 0, false}); break;
    case 7: invalid.wakes.resize(LevelTriggers::Checkpoint::kMaxEvents + 1); break;
    case 8: invalid.frameRemainder = 1; break;
    case 9: invalid.emptyToggleDelay = -1; break;
    case 10: invalid.playerCount = 5; break;
    case 11: invalid.triggers[0].radius += 1; break;         // bounded but wrong native roster
    case 12: invalid.figures[2] = invalid.figures[0]; break; // marker cannot gain art
    case 13: invalid.targets[0].openHeight += 1; break;
    }
    CHECK_FALSE(fixture.triggers.prepareRestore(invalid));
    CHECK_FALSE(fixture.triggers.restore(invalid));
    CHECK(fixture.state() == before);
}

TEST_CASE("native campaign trigger owners retain their complete loaded roster and figures",
          "[triggers-checkpoint][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto name = GENERATE("G1", "A1", "J4");
    CAPTURE(name);
    const auto level = catalog.byName(name);
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    WorldAnimator animator;
    animator.bind(world.layout());
    WorldCollision collision = world.collision();
    LevelTriggers owner;
    owner.bind(world.layout(), animator, &collision);
    owner.bindFigures(device, world.layout(), world.items(), &world.realmItems());
    const auto state = owner.checkpoint();
    REQUIRE(state);
    REQUIRE_FALSE(state->triggers.empty());
    REQUIRE(state->figures.size() == state->triggers.size());
    const auto uploads = device.texturesCreated;
    auto prepared = owner.prepareRestore(*state);
    REQUIRE(prepared);
    owner.setPlayerCount(4);
    owner.setCameraHeld(true);
    for (usize index = 0; index < owner.size(); ++index) {
        owner.shoot(index);
    }
    REQUIRE(owner.commitRestore(std::move(*prepared)));
    CHECK(owner.checkpoint() == state);
    CHECK(device.texturesCreated == uploads);
}
} // namespace
