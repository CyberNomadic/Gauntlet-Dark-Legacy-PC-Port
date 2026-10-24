#include <algorithm>
#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/level/LevelCatalog.h"
#include "game/level/SpawnedObjects.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path unpackedRoot() {
    return test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2").parent_path().parent_path().parent_path();
}

TEST_CASE("an object's tree is the object at the root with its descendants at their offsets",
          "[level][spawned-objects][dev-console][assets]") {
    const auto root = unpackedRoot(); // before any assertion: a skip inside one fails it
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELA1"));
    const auto& objects = layout.objects();
    const auto parent = std::ranges::find_if(objects, [&](const WorldObject& object) {
        return object.child >= 0 && static_cast<usize>(object.child) < objects.size();
    });
    REQUIRE(parent != objects.end());
    const auto index = static_cast<usize>(parent - objects.begin());
    const TreeInfo tree = SpawnedObjects::treeOf(layout, index);
    CHECK(tree.name == parent->name);
    REQUIRE(tree.nodes.size() >= 2);
    CHECK(tree.nodes[0].object == parent->name);
    CHECK(tree.nodes[0].parent == -1);
    CHECK(tree.nodes[0].position == Vec3{0.0f, 0.0f, 0.0f});
    const WorldObject& child = objects[static_cast<usize>(parent->child)];
    CHECK(tree.nodes[1].object == child.name);
    CHECK(tree.nodes[1].parent == 0);
    CHECK(tree.nodes[1].position == child.position);
    // Every node but the root hangs off an earlier one.
    for (usize i = 1; i < tree.nodes.size(); ++i) {
        CHECK(tree.nodes[i].parent >= 0);
        CHECK(static_cast<usize>(tree.nodes[i].parent) < i);
    }
    CHECK(SpawnedObjects::treeOf(layout, objects.size()).nodes.empty());
    CHECK(SpawnedObjects::objectNamed(layout, parent->name) == index);
    std::string lowered = parent->name;
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    CHECK(SpawnedObjects::objectNamed(layout, lowered) == index);
    CHECK_FALSE(SpawnedObjects::objectNamed(layout, "NOTHING_OF_THE_KIND").has_value());
}

TEST_CASE("objects are stood out of their own level's archive, kept once per level, and drawn",
          "[level][spawned-objects][dev-console][assets]") {
    const auto root = unpackedRoot();
    test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto castle = catalog.byName("A1");
    const auto town = catalog.byName("G1");
    REQUIRE(castle.has_value());
    REQUIRE(town.has_value());
    // An object with a mesh of its own: a group's name has none, and a marker's is never drawn.
    const auto meshed = [&](const LevelRef& level, WorldLayout& layout) {
        REQUIRE(layout.load(root / level.directory));
        ModelSet models;
        REQUIRE(models.load(root / level.directory));
        const auto found = std::ranges::find_if(layout.objects(), [&](const WorldObject& object) {
            return !object.particles() && models.find(object.name).has_value();
        });
        REQUIRE(found != layout.objects().end());
        return found->name;
    };
    WorldLayout townLayout;
    const std::string townObject = meshed(*town, townLayout);
    test::FakeRenderDevice device;
    SpawnedObjects spawned;
    CHECK(spawned.count() == 0);
    CHECK_FALSE(spawned.spawn(device, root, *town, "NOTHING_OF_THE_KIND", Mat4{1.0f}));
    CHECK(spawned.sourceCount() == 1); // the level is loaded for the asking, and kept
    REQUIRE(spawned.spawn(device, root, *town, townObject, Mat4{1.0f}));
    CHECK(spawned.count() == 1);
    CHECK(spawned.sourceCount() == 1);
    device.draws.clear();
    spawned.draw(device, Mat4{1.0f}, WorldLighting{});
    const usize oneDrawn = device.draws.size();
    CHECK(oneDrawn > 0);
    WorldLayout castleLayout;
    const std::string castleObject = meshed(*castle, castleLayout);
    const Mat4 away = glm::translate(Mat4{1.0f}, Vec3{10.0f, 0.0f, 20.0f});
    REQUIRE(spawned.spawn(device, root, *castle, castleObject, away));
    CHECK(spawned.count() == 2);
    CHECK(spawned.sourceCount() == 2);
    device.draws.clear();
    spawned.draw(device, Mat4{1.0f}, WorldLighting{});
    CHECK(device.draws.size() > oneDrawn); // both are drawn
    spawned.clear();
    CHECK(spawned.count() == 0);
    CHECK(spawned.sourceCount() == 0);
}

} // namespace
