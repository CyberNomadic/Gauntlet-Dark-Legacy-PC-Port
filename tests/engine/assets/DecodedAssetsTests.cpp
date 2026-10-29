#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Error.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"

#include "FakeRenderDevice.h"
#include "formats/AnimationTree.h"

namespace {
using namespace gdl;

Mesh triangle() {
    Mesh mesh;
    mesh.vertices = {{.position = {0, 0, 0}}, {.position = {1, 0, 0}}, {.position = {0, 1, 0}}};
    mesh.parts.push_back({.indices = {0, 1, 2}});
    return mesh;
}

TEST_CASE("Decoded native assets use the shared tree renderer without filesystem exports",
          "[assets-decoded][legends]") {
    ItemArchive archive;
    archive.models = ModelSet::fromDecoded({{.name = "piece"}}, {triangle()});
    archive.textures = TextureSet::fromDecoded({{.name = "skin", .width = 2, .height = 2}},
                                               {Image::filled(2, 2, Color::white())});
    formats::AnimationFile animations;
    formats::TreeDefinition tree;
    tree.name = "fixture";
    tree.nodes.push_back({.name = "ROOT", .object = "piece"});
    formats::TreeSequence sequence;
    sequence.name = "IDLE";
    sequence.frameCount = 30;
    sequence.frameRate = 30;
    sequence.tracks.push_back({.node = 0, .flags = 0x10, .frames = {0, 29}, .values = {0, 10}});
    tree.sequences.push_back(std::move(sequence));
    animations.trees.push_back(std::move(tree));
    archive.trees = AnimationSet::fromDecoded(std::move(animations));
    REQUIRE(archive.models.find("PIECE") == 0);
    REQUIRE(archive.textures.find("SKIN") == 0);
    REQUIRE(archive.trees.find("FIXTURE") == 0);
    CHECK(archive.models.entry(0).triangles == 1);
    test::FakeRenderDevice device;
    TreeModel model;
    REQUIRE(model.bind(archive.trees.tree(0), archive.models, archive.textures, device));
    TreePose pose;
    pose.evaluate(archive.trees.tree(0), 0, 29);
    model.draw(device, Mat4{1}, Mat4{1}, {}, pose.matrices());
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws[0].vertices[0].position.x == 10);
    model.clear();
    archive.clear();
    CHECK_FALSE(archive.loaded());
}

TEST_CASE("Decoded asset factories reject mismatched or malformed native input",
          "[assets-decoded][legends]") {
    CHECK_THROWS_AS(ModelSet::fromDecoded({{.name = "piece"}}, {}), FormatError);
    CHECK_THROWS_AS(
        ModelSet::fromDecoded({{.name = "piece"}, {.name = "PIECE"}}, {triangle(), triangle()}),
        FormatError);
    auto mesh = triangle();
    mesh.parts[0].indices[2] = 8;
    CHECK_THROWS_AS(ModelSet::fromDecoded({{.name = "piece"}}, {mesh}), FormatError);
    CHECK_THROWS_AS(TextureSet::fromDecoded({{.name = "skin"}}, {}), FormatError);
    CHECK_THROWS_AS(TextureSet::fromDecoded({{.name = "skin"}}, {Image{}}), FormatError);
    CHECK_THROWS_AS(TextureSet::fromDecoded({{.name = "skin", .width = 4, .height = 4}},
                                            {Image::filled(2, 2, Color::white())}),
                    FormatError);
    auto external = TextureSet::fromDecoded(
        {{.name = "borrowed", .flags = TextureSetEntry::kExternal}}, {Image{}});
    CHECK_THROWS_AS(external.image(0), FileError);
    auto empty = TextureSet::fromDecoded({{.name = "slot", .noPicture = true}}, {Image{}});
    CHECK(empty.image(0).pixels == std::vector<u8>{0, 0, 0, 0});
}

TEST_CASE("Decoded world metadata retains native starts without inventing GameCube fixtures",
          "[assets-decoded][legends][imported-level]") {
    const WorldLocator start{
        .kind = LocatorKind::Start, .position = {4, 2, 7}, .rotation = {0, 1, 0}};
    const auto empty = WorldLayout::fromDecoded({}, {start}, {-10, -5, -20}, {10, 8, 20});
    CHECK_FALSE(empty.loaded());
    REQUIRE(empty.findLocator(LocatorKind::Start));
    CHECK(empty.findLocator(LocatorKind::Start)->position == start.position);
    CHECK(empty.itemInfos().empty());
    CHECK(empty.itemInstances().empty());
    CHECK(empty.animations().empty());
    CHECK(empty.particleTemplates().empty());
    CHECK(empty.minBounds() == Vec3{-10, -5, -20});
    CHECK(empty.maxBounds() == Vec3{10, 8, 20});

    const auto hierarchy =
        WorldLayout::fromDecoded({{.name = "root", .position = {1, 0, 0}, .child = 1},
                                  {.name = "child", .position = {0, 2, 0}, .next = 2},
                                  {.name = "sibling", .position = {0, 0, 3}, .parent = 100}},
                                 {start}, {-10, -5, -20}, {10, 8, 20});
    REQUIRE(hierarchy.loaded());
    CHECK(hierarchy.objects()[1].parent == 0);
    CHECK(hierarchy.objects()[2].parent == 0);
    CHECK(hierarchy.worldPosition(1) == Vec3{1, 2, 0});
    CHECK(hierarchy.worldPosition(2) == Vec3{1, 0, 3});
}

TEST_CASE("Decoded world metadata rejects invalid coordinates and cyclic ownership",
          "[assets-decoded][legends][imported-level]") {
    const auto invalid = std::numeric_limits<f32>::quiet_NaN();
    CHECK_THROWS_AS(WorldLayout::fromDecoded({}, {}, {invalid, 0, 0}, Vec3{10}), FormatError);
    CHECK_THROWS_AS(WorldLayout::fromDecoded({}, {}, {1, 0, 0}, Vec3{0}), FormatError);
    CHECK_THROWS_AS(
        WorldLayout::fromDecoded({{.position = {invalid, 0, 0}}}, {}, Vec3{0}, Vec3{10}),
        FormatError);
    CHECK_THROWS_AS(WorldLayout::fromDecoded({{.radius = -1}}, {}, Vec3{0}, Vec3{10}), FormatError);
    CHECK_THROWS_AS(WorldLayout::fromDecoded({{.child = 1}}, {}, Vec3{0}, Vec3{10}), FormatError);
    CHECK_THROWS_AS(WorldLayout::fromDecoded({{.next = -2}}, {}, Vec3{0}, Vec3{10}), FormatError);
    CHECK_THROWS_AS(WorldLayout::fromDecoded({{.child = 0}}, {}, Vec3{0}, Vec3{10}), FormatError);
    CHECK_THROWS_AS(WorldLayout::fromDecoded({{.child = 1}, {.child = 0}}, {}, Vec3{0}, Vec3{10}),
                    FormatError);
    CHECK_THROWS_AS(
        WorldLayout::fromDecoded({{.child = 2}, {.child = 2}, {}}, {}, Vec3{0}, Vec3{10}),
        FormatError);
    CHECK_THROWS_AS(WorldLayout::fromDecoded({{.next = 1}, {.next = 0}}, {}, Vec3{0}, Vec3{10}),
                    FormatError);
    CHECK_THROWS_AS(WorldLayout::fromDecoded({}, {{.kind = LocatorKind::Count}}, Vec3{0}, Vec3{10}),
                    FormatError);
    CHECK_THROWS_AS(
        WorldLayout::fromDecoded({}, {{.rotation = {0, invalid, 0}}}, Vec3{0}, Vec3{10}),
        FormatError);
}
} // namespace
