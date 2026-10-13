#include <filesystem>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AssetScope.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"

namespace {

using namespace gdl;

/** An archive of one triangle under the object names given, with one tree named `tree`. */
std::filesystem::path archiveWith(std::string_view name, std::string_view tree,
                                  std::string_view objects) {
    const auto dir = test::scratchDirectory(name);
    std::filesystem::create_directories(dir / "models");
    std::filesystem::create_directories(dir / "textures");
    writeTextFile(dir / "models/body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json",
                  std::string{R"({"objects": [)"} + std::string{objects} + "]}");
    writeFile(dir / "textures/skin.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({"defs": [], "bitmaps": [
  {"index": 0, "name": "SKIN", "file": "textures/skin.png", "width": 2, "height": 2, "flags": 0}]})");
    test::convertModelFixture(dir);
    writeTextFile(dir / "animations.json", std::string{R"({"trees": [{"name": ")"} +
                                               std::string{tree} +
                                               R"(", "prefix": "", "sequences": [], "nodes": [
    {"name": "BODY", "object": "BODY", "type": 0, "flags": 0, "objectFlags": 0, "parent": -1,
     "position": [0, 0, 0]}]}]})");
    return dir;
}

TEST_CASE("an asset scope searches its archives in order, trees by name and objects by suffix",
          "[assets][asset-scope]") {
    const auto levelDir = archiveWith("asset-scope-level", "TENTACLE", R"(
  {"index": 0, "name": "BODY", "file": "models/body.obj", "meshTriangles": 1},
  {"index": 1, "name": "ROCK0L1", "file": "models/body.obj", "meshTriangles": 1},
  {"index": 2, "name": "SHARED", "file": "models/body.obj", "meshTriangles": 1})");
    const auto realmDir = archiveWith("asset-scope-realm", "BRIDGEPAD", R"(
  {"index": 0, "name": "BODY", "file": "models/body.obj", "meshTriangles": 1},
  {"index": 1, "name": "ROCK0", "file": "models/body.obj", "meshTriangles": 1},
  {"index": 2, "name": "GATE1L1ROOT", "file": "models/body.obj", "meshTriangles": 1},
  {"index": 3, "name": "SHARED", "file": "models/body.obj", "meshTriangles": 1})");
    ItemArchive level;
    ItemArchive realm;
    ItemArchive unloaded;
    REQUIRE(level.load(levelDir));
    REQUIRE(realm.load(realmDir));

    AssetScope scope;
    scope.add(level);
    scope.add(unloaded); // never joins the search
    scope.add(realm);
    scope.add(level); // never twice
    REQUIRE(scope.archives().size() == 2);
    REQUIRE(scope.primary() == &level);
    REQUIRE(scope.textureLenders() == std::vector<TextureSet*>{&level.textures, &realm.textures});

    // Trees come from the first archive that names them.
    REQUIRE(scope.findTree("TENTACLE").has_value());
    CHECK(scope.findTree("TENTACLE")->archive == &level);
    REQUIRE(scope.findTree("BRIDGEPAD").has_value());
    CHECK(scope.findTree("BRIDGEPAD")->archive == &realm);
    CHECK_FALSE(scope.findTree("TRIGGER2").has_value());
    CHECK(scope.archiveForTree("BRIDGEPAD") == &realm);
    CHECK(scope.archiveForTree("TRIGGER2") == &level); // the primary stands in

    // Objects: the plain spelling in any archive beats a suffixed one in the first.
    const auto shared = scope.findObject("SHARED");
    REQUIRE(shared.has_value());
    CHECK(shared->archive == &level);
    const auto rock = scope.findObject("ROCK0");
    REQUIRE(rock.has_value());
    CHECK(rock->archive == &realm);
    CHECK(rock->name == "ROCK0");
    const auto gate = scope.findObject("GATE1");
    REQUIRE(gate.has_value());
    CHECK(gate->archive == &realm);
    CHECK(gate->name == "GATE1L1ROOT");
    CHECK_FALSE(scope.findObject("GATE2").has_value());

    // Alone, the level archive still resolves its own suffixed object.
    const AssetScope only(level);
    REQUIRE(only.findObject("ROCK0").has_value());
    CHECK(only.findObject("ROCK0")->name == "ROCK0L1");
    CHECK(only.archiveForTree("BRIDGEPAD") == &level);

    const AssetScope none;
    CHECK(none.empty());
    CHECK(none.primary() == nullptr);
    CHECK(none.archiveForTree("TENTACLE") == nullptr);
    CHECK_FALSE(none.findObject("ROCK0").has_value());
}

} // namespace
