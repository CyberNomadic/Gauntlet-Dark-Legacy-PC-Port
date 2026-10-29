#include <array>
#include <set>

#include <catch2/catch_test_macros.hpp>

#include "../../engine/world/SampleLevel.h"
#include "FakeRenderDevice.h"
#include "engine/assets/AssetScope.h"
#include "engine/io/File.h"
#include "game/level/Chests.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** A level with one random (silver) chest choosing among three gold records, a chest holding
 * junk outright, and a realm archive lending the gilded chest trees. */
struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    ItemArchive realm;
    Chests chests;

    explicit Fixture(std::string_view name) {
        const auto directory = test::sampleLevel(name);
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"WALL","position":[0,0,0]}],
          "itemInfos":[
            {"type":2,"subtype":48,"name":"CHEST","radius":1,"height":2,"activeType":16},
            {"type":-1,"choices":[2,3,4]},
            {"type":1,"subtype":1,"name":"JUNK","radius":1,"value":5},
            {"type":1,"subtype":1,"name":"TREAS_SILVER","radius":1,"value":100},
            {"type":1,"subtype":1,"name":"TREAS_GOLD","radius":1,"value":200},
            {"type":2,"subtype":46,"name":"CHEST","radius":1,"height":2,"activeType":16}],
          "itemInstances":[
            {"info":0,"position":[0,0,0],"params":[1,0,0,0,1,0,0,0,0,0,0,0]},
            {"info":5,"position":[20,0,0],"params":[2,0,0,0,1,0,0,0,0,0,0,0]}]})");
        writeTextFile(directory / "animations.json", R"({"trees":[
          {"name":"CHEST","nodes":[
             {"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]},
             {"name":"NULL1","parent":0,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":2,"frameRate":30},
             {"name":"ACTIVE","frames":8,"frameRate":30},
             {"name":"OPEN","frames":0}]},
          {"name":"JUNK","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":8,"frameRate":30}]},
          {"name":"TREAS_SILVER","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":8,"frameRate":30}]},
          {"name":"TREAS_GOLD","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":8,"frameRate":30}]}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(items.load(directory));
        // The realm's archive is where the gilded chests live, after the level's own.
        const auto realmDirectory = test::sampleLevel(std::string(name) + "-realm");
        writeTextFile(realmDirectory / "animations.json", R"({"trees":[
          {"name":"CHESTG1","nodes":[
             {"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]},
             {"name":"NULL1","parent":0,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":2,"frameRate":30},
             {"name":"ACTIVE","frames":8,"frameRate":30},
             {"name":"OPEN","frames":0}]}]})");
        REQUIRE(realm.load(realmDirectory));
        REQUIRE(chests.bind(device, layout, AssetScope(items, &realm), nullptr));
        chests.setPlayerCount(1);
        REQUIRE(chests.size() == 2);
    }
    AssetScope scope() { return AssetScope(items, &realm); }
};

TEST_CASE("a random chest cycles through its choices with the item clock until it is opened",
          "[game][level][chests][random-chest]") {
    Fixture f("chests-random-cycle");
    constexpr f32 kFrame = 1.0f / 30.0f;
    const std::array watcher{ChestVisitor{Vec3{0}, 1, 0, true}};
    // The X-ray overlay follows the pick: as the clock runs, it shows each choice in turn.
    std::set<const TreeInfo*> shown;
    s32 changes = 0;
    const TreeInfo* last = nullptr;
    for (s32 frame = 0; frame < 32 * 4; ++frame) {
        f.chests.update(kFrame, {});
        f.chests.updateXray(f.device, f.scope(), f.items, watcher);
        const TreeInfo* preview = f.chests.chest(0).preview.tree();
        REQUIRE(preview != nullptr);
        if (preview != last) {
            ++changes;
            last = preview;
        }
        shown.insert(preview);
    }
    CHECK(shown.size() == 3);
    CHECK(changes >= 3);
    // Half a frame leaves its fraction for the next update: no tick is lost or doubled.
    Chests::LocalSnapshot before;
    f.chests.saveLocal(before);
    f.chests.update(kFrame * 0.5f, {});
    f.chests.update(kFrame * 0.5f, {});
    Chests::LocalSnapshot after;
    f.chests.saveLocal(after);
    CHECK_FALSE(f.chests.equalLocal(before, after));
}

TEST_CASE("gilding a chest's junk dresses it in the realm's gold chest and refreshes the X-ray",
          "[game][level][chests][magic-perks]") {
    Fixture f("chests-gilded");
    const std::array watcher{ChestVisitor{Vec3{20, 0, 0}, 1, 0, true}};
    f.chests.update(0.0f, {});
    REQUIRE(f.chests.updateXray(f.device, f.scope(), f.items, watcher) == 1);
    REQUIRE(f.chests.chest(1).preview.tree() != nullptr);
    CHECK(f.chests.chest(1).preview.tree()->name == "JUNK");
    REQUIRE(f.chests.chest(1).figure.tree() != nullptr);
    CHECK(f.chests.chest(1).figure.tree()->name == "CHEST");
    REQUIRE(f.chests.changeContents(1, 3, f.device, f.layout, f.scope(), nullptr, "CHESTG1"));
    CHECK(f.chests.chest(1).contents == 3);
    REQUIRE(f.chests.chest(1).figure.tree() != nullptr);
    CHECK(f.chests.chest(1).figure.tree()->name == "CHESTG1");
    // The overlay shows the silver the moment it is looked at again.
    f.chests.updateXray(f.device, f.scope(), f.items, watcher);
    REQUIRE(f.chests.chest(1).preview.tree() != nullptr);
    CHECK(f.chests.chest(1).preview.tree()->name == "TREAS_SILVER");
}

TEST_CASE("the developer console decides whether a shut chest wants a key",
          "[game][level][chests][dev-console]") {
    Fixture f("chests-console-lock");
    REQUIRE(f.chests.chest(0).locked);
    const std::array keyless{ChestVisitor{Vec3{0}, 1, 0}};
    f.chests.update(0, keyless);
    CHECK(f.chests.chest(0).state == Chests::kShut); // refused without a key
    REQUIRE(f.chests.setLocked(0, false));
    CHECK_FALSE(f.chests.chest(0).locked);
    f.chests.update(3, keyless); // the refusal's pause is over: it opens without one
    CHECK(f.chests.chest(0).state == Chests::kOpening);
    CHECK_FALSE(f.chests.setLocked(0, true)); // open already: a key no longer matters
    REQUIRE(f.chests.setLocked(1, true));
    CHECK(f.chests.chest(1).locked);
    REQUIRE(f.chests.setLocked(1, false));
    CHECK_FALSE(f.chests.chest(1).locked);
    CHECK_FALSE(f.chests.setLocked(7, true));
}

} // namespace
