#include <cmath>
#include <filesystem>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "formats/LegendsDreamcastTexture.h"
#include "formats/LegendsN64Texture.h"
#include "game/scenes/LegendsPreview.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

formats::LegendsMap triangleMap() {
    formats::LegendsMap map;
    formats::LegendsMapObject object;
    object.name = "test";
    object.mesh.vertices = {{.position = {0, 0, 0}, .uv = {0, 0}},
                            {.position = {10, 0, 0}, .uv = {2, 0}},
                            {.position = {0, 0, 10}, .uv = {0, 2}}};
    object.mesh.parts.push_back({.texture = 0, .indices = {0, 1, 2}});
    object.transform = glm::translate(Mat4{1}, Vec3{3, 5, 7});
    map.objects.push_back(std::move(object));
    return map;
}

void render(LegendsPreview& preview, test::FakeRenderDevice& device) {
    preview.render(device, Mat4{1}, 640, 448, kPi / 2);
}

TEST_CASE("A fallen character cannot replace a Legends visit with its restored entry save",
          "[legends][preview][save]") {
    test::FakeRenderDevice device;
    LegendsPreview preview;
    const auto map = triangleMap();
    preview.open(device, map);
    const auto camera = preview.camera();
    PartyMember member;
    member.fallen = true;
    REQUIRE_THROWS_WITH(preview.openPlayable(device, map, {}, member),
                        "Legends traversal: select a living character before entering");
    CHECK(preview.isOpen());
    CHECK_FALSE(preview.playable());
    CHECK(preview.camera() == camera);
    CHECK(preview.triangleCount() == 1);
}

TEST_CASE("Legends map preview uses native placement and keeps the game-independent fly camera",
          "[legends][preview]") {
    test::FakeRenderDevice device;
    LegendsPreview preview;
    CHECK_FALSE(preview.isOpen());
    auto map = triangleMap();
    preview.open(device, map);
    CHECK(preview.isOpen());
    CHECK(preview.triangleCount() == 1);
    render(preview, device);
    REQUIRE(device.draws.size() == 1);
    const auto& draw = device.draws.front();
    REQUIRE(draw.vertices.size() == 3);
    CHECK(draw.vertices[0].position == Vec3{3, 5, 7});
    CHECK(draw.vertices[1].position == Vec3{13, 5, 7});
    CHECK(draw.vertices[2].position == Vec3{3, 5, 17});
    CHECK(draw.texture == &device.whiteTexture());
    CHECK(draw.state.depthTest);
    CHECK(draw.state.depthWrite);
    CHECK_FALSE(draw.state.cullBack);

    const Vec3 centre{8, 5, 12};
    CHECK(glm::dot(glm::normalize(centre - preview.camera().position),
                   preview.camera().forward()) == Approx(1).margin(0.0001));
    Input input;
    input.beginPoll();
    input.setKey(Key::W, true);
    const auto start = preview.camera();
    preview.update(input, 0.1f, true);
    CHECK(preview.camera() == start);
    preview.update(input, 0.1f, false);
    CHECK(preview.camera().position != start.position);
    preview.close();
    CHECK_FALSE(preview.isOpen());
    CHECK(preview.triangleCount() == 0);
    device.draws.clear();
    render(preview, device);
    CHECK(device.draws.empty());
}

TEST_CASE("Legends preview rejects malformed replacements without losing its current map",
          "[legends][preview]") {
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.open(device, triangleMap());
    const auto camera = preview.camera();
    auto map = triangleMap();
    SECTION("invalid index") {
        map.objects[0].mesh.parts[0].indices[1] = 3;
    }
    SECTION("incomplete triangle") {
        map.objects[0].mesh.parts[0].indices.pop_back();
    }
    SECTION("nonfinite position") {
        map.objects[0].mesh.vertices[0].position.x = std::numeric_limits<f32>::infinity();
    }
    SECTION("nonfinite texture coordinate") {
        map.objects[0].mesh.vertices[0].uv.y = std::numeric_limits<f32>::quiet_NaN();
    }
    SECTION("finite coordinates overflow the camera bounds") {
        map.objects[0].mesh.vertices[0].position.x = std::numeric_limits<f32>::max();
    }
    SECTION("no geometry") {
        map.objects.clear();
    }
    SECTION("invalid texture byte count") {
        map.textures.push_back({.image = {.width = 4, .height = 4, .pixels = {255}}});
    }
    SECTION("singular billboard placement") {
        map.objects[0].nativeDrawFlags = 0x01000000;
        map.objects[0].transform[0] = Vec4{0};
    }
    CHECK_THROWS_AS(preview.open(device, map), FormatError);
    CHECK(preview.isOpen());
    CHECK(preview.triangleCount() == 1);
    CHECK(preview.camera() == camera);
    render(preview, device);
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws.front().vertices[0].position == Vec3{3, 5, 7});
}

TEST_CASE("Legends scenery keeps separate native objects when they share a texture",
          "[legends][preview]") {
    auto map = triangleMap();
    auto second = map.objects.front();
    second.name = "second";
    second.transform = glm::translate(Mat4{1}, Vec3{30, 5, 7});
    map.objects.push_back(std::move(second));
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.open(device, map);
    render(preview, device);
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[0].texture == device.draws[1].texture);
    CHECK(device.draws[0].vertices[0].position == Vec3{3, 5, 7});
    CHECK(device.draws[1].vertices[0].position == Vec3{30, 5, 7});
    CHECK(preview.triangleCount() == 2);
}

TEST_CASE(
    "Legends preview uses native texture slots and texel coordinates without borrowing GC flags",
    "[legends][preview]") {
    test::FakeRenderDevice device;
    LegendsPreview preview;
    auto map = triangleMap();
    map.textures.push_back({.image = Image::filled(2, 2, Color::white()), .clampU = true});
    auto& image = map.textures[0].image;
    SECTION("opaque") {}
    SECTION("binary alpha") {
        image.pixels[3] = 0;
    }
    SECTION("soft alpha") {
        image.pixels[3] = 127;
    }
    preview.open(device, map);
    render(preview, device);
    REQUIRE(device.draws.size() == 1);
    const auto& draw = device.draws[0];
    REQUIRE(draw.texture != &device.whiteTexture());
    CHECK(draw.texture->width() == 2);
    CHECK(draw.vertices[1].uv.x == Approx(1));
    CHECK(draw.vertices[2].uv.y == Approx(1));
    CHECK(device.lastTextureDesc.wrap == TextureWrap::ClampToEdge);
    CHECK(device.lastTextureDesc.wrapDown() == TextureWrap::Repeat);
    CHECK(device.lastTextureDesc.generateMipmaps);
    CHECK(draw.state.alphaTest == (image.pixels[3] < 255 ? DrawState::kTranslucentAlphaTest : 0));
    CHECK(draw.state.depthWrite == (image.pixels[3] != 127));
    CHECK(draw.state.blend == (image.pixels[3] == 127 ? BlendMode::Alpha : BlendMode::Opaque));
}

TEST_CASE("N64 facing scenery rotates around its own anchor without moving the world",
          "[legends][preview][world-presentation]") {
    auto map = triangleMap();
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.open(device, map);
    render(preview, device);
    REQUIRE(device.draws.size() == 1);
    const auto fixed = device.draws.front();
    const auto camera = preview.camera();
    device.draws.clear();
    map.objects[0].nativeDrawFlags = 0x09001800;
    preview.open(device, map);
    render(preview, device);
    REQUIRE(device.draws.size() == 1);
    const auto& faced = device.draws.front();
    CHECK(preview.camera() == camera);
    CHECK(faced.vertices == fixed.vertices);
    CHECK(faced.transform != fixed.transform);
    const Vec4 anchor{3, 5, 7, 1};
    CHECK(glm::length(faced.transform * anchor - fixed.transform * anchor) < .001f);
    CHECK(glm::length(faced.transform * Vec4{13, 5, 7, 1} - fixed.transform * Vec4{13, 5, 7, 1}) >
          .1f);
}

TEST_CASE("N64 world draw flags do not borrow collision flag semantics",
          "[legends][preview][world-presentation]") {
    auto map = triangleMap();
    map.objects.front().flags = 2; // Not the native drawable's hidden bit.
    map.objects.front().nativeDrawFlags = 0x100000c0;
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.open(device, map);
    render(preview, device);
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws.front().state.blend == BlendMode::Alpha);
    CHECK_FALSE(device.draws.front().state.depthWrite);
    CHECK_FALSE(device.draws.front().state.depthTest);
    device.draws.clear();
    map.objects.front().nativeDrawFlags |= 2;
    preview.open(device, map);
    render(preview, device);
    CHECK(device.draws.empty());
}

TEST_CASE("Legends preview does not divide by a minimized viewport or invalid field of view",
          "[legends][preview]") {
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.open(device, triangleMap());
    preview.render(device, Mat4{1}, 640, 0, kPi / 2);
    preview.render(device, Mat4{1}, 0, 448, kPi / 2);
    preview.render(device, Mat4{1}, 640, 448, 0);
    preview.render(device, Mat4{1}, 640, 448, kPi);
    CHECK(device.draws.empty());
}

TEST_CASE("Legends walking renders the selected native character and holds input under the console",
          "[legends][preview][assets]") {
    const auto root = test::assetOrSkip("PLAYERS").parent_path();
    auto map = triangleMap();
    map.collision = {{.vertices = {Vec3{-20, 0, -20}, Vec3{-20, 0, 20}, Vec3{20, 0, -20}},
                      .owner = 0,
                      .floor = true},
                     {.vertices = {Vec3{20, 0, -20}, Vec3{-20, 0, 20}, Vec3{20, 0, 20}},
                      .owner = 0,
                      .floor = true}};
    map.starts.push_back({.position = {0, 0, 0}});
    PartyMember member;
    member.save.name = "TEST";
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.openPlayable(device, map, root, member);
    REQUIRE(preview.playable());
    REQUIRE(preview.traversal());
    const auto initial = preview.traversal()->actor().position();
    Input input;
    input.beginPoll();
    input.setKey(Key::W, true);
    const GameConfig config;
    preview.updatePlayable(input, 0.1f, true, config);
    CHECK(preview.traversal()->actor().position() == initial);
    for (s32 frame = 0; frame < 60; ++frame) {
        preview.updatePlayable(input, 1.0f / 60.0f, false, config);
    }
    CHECK(glm::distance(preview.traversal()->actor().position(), initial) > 1);
    render(preview, device);
    CHECK(device.draws.size() > 1);
    CHECK(member.save.name == "TEST");
    preview.open(device, triangleMap());
    CHECK_FALSE(preview.playable());
    CHECK(preview.traversal() == nullptr);
    preview.close();
    CHECK_FALSE(preview.isOpen());
}

TEST_CASE("native Legends maps reach the renderer without conversion or GameCube level state",
          "[legends][preview][assets]") {
    const auto assets = test::dataDirectory().parent_path() / "assets";
    const auto survey = [&](const std::filesystem::path& levels, bool n64, usize expected) {
        if (!std::filesystem::is_directory(levels)) {
            SKIP("native Legends extraction is not installed");
        }
        test::FakeRenderDevice device;
        LegendsPreview preview;
        usize count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(levels)) {
            const auto world = entry.path() / (n64 ? "worlds.wad" : "worlds.rom");
            const auto models = entry.path() / (n64 ? "objects.wad" : "objects.rom");
            if (!std::filesystem::is_regular_file(world) ||
                !std::filesystem::is_regular_file(models) ||
                std::filesystem::file_size(world) == 0) {
                continue;
            }
            INFO(entry.path().string());
            const auto modelBytes = readFile(models);
            auto map = n64 ? formats::LegendsMap::parseN64(readFile(world), modelBytes)
                           : formats::LegendsMap::parseDreamcast(readFile(world), modelBytes);
            if (n64) {
                for (auto& texture : formats::decodeLegendsN64Textures(modelBytes)) {
                    map.textures.push_back({std::move(texture.image), texture.clampU,
                                            texture.clampV, texture.hasTransparency});
                }
            }
            if (!n64) {
                map.textures = formats::decodeLegendsDreamcastTextures(
                    modelBytes, readFile(entry.path() / "textures.rom"));
            }
            preview.open(device, map);
            REQUIRE(preview.isOpen());
            CHECK(preview.triangleCount() > 0);
            device.draws.clear();
            render(preview, device);
            usize submitted = 0;
            for (const auto& draw : device.draws) {
                submitted += draw.vertices.size() / 3;
                CHECK(draw.state.depthTest);
                for (int column = 0; column < 4; ++column) {
                    for (int row = 0; row < 4; ++row) {
                        REQUIRE(std::isfinite(draw.transform[column][row]));
                    }
                }
            }
            CHECK(submitted == preview.triangleCount());
            ++count;
        }
        CHECK(count == expected);
    };
    SECTION("Nintendo 64") {
        survey(assets / "n64/extracted/levels", true, 34);
    }
    SECTION("Dreamcast") {
        survey(assets / "dreamcast/extracted/native/levels", false, 41);
    }
}
} // namespace
