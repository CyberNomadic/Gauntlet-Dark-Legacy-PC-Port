#include <algorithm>
#include <filesystem>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/TextureSet.h"
#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "formats/GcTexture.h"
#include "formats/ModelArchive.h"

namespace {

using namespace gdl;

std::filesystem::path sampleSet(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    std::filesystem::create_directories(dir / "textures");
    writeFile(dir / "textures/000_GLOW_.png", test::kTinyPng);
    writeFile(dir / "textures/001_GLOW_+1.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({
  "source": "/sample/",
  "defs": [
    {"name": "GLOW_", "index": 0, "width": 2, "height": 2},
    {"name": "ALIAS", "index": 1, "width": 2, "height": 2}
  ],
  "bitmaps": [
    {"index": 0, "name": "GLOW_", "file": "textures/000_GLOW_.png", "width": 2, "height": 2,
     "format": 50, "flags": 141, "halfResolution": true, "frames": 2, "clampU": true},
    {"index": 1, "name": "GLOW_+1", "file": "textures/001_GLOW_+1.png", "width": 2, "height": 2,
     "format": 50, "flags": 12, "halfResolution": false, "frames": 0}
  ]
})");
    return dir;
}

TEST_CASE("a texture set reads its manifest and resolves names", "[assets][textures]") {
    TextureSet set;
    REQUIRE(set.load(sampleSet("texture-set")));
    REQUIRE(set.loaded());
    REQUIRE(set.size() == 2);
    REQUIRE(set.find("glow_") == 0U);
    REQUIRE(set.find("GLOW_+1") == 1U);
    REQUIRE(set.find("alias") == 1U);
    REQUIRE_FALSE(set.find("nothing").has_value());

    const TextureSetEntry& glow = set.entry(0);
    REQUIRE(glow.name == "GLOW_");
    REQUIRE(glow.width == 2);
    REQUIRE(glow.flags == 141);
    REQUIRE(glow.frames == 2);
    REQUIRE(glow.halfResolution);
    REQUIRE_FALSE(set.entry(1).halfResolution);

    const Image& image = set.image(1);
    REQUIRE(image.width == 2);
    REQUIRE(image.pixel(0, 0) == Color::rgba(255, 0, 0, 255));
}

TEST_CASE("GPU textures are created once and released on demand", "[assets][textures]") {
    TextureSet set;
    REQUIRE(set.load(sampleSet("texture-set-gpu")));
    test::FakeRenderDevice device;
    const Texture& first = set.texture(device, 0);
    const Texture& again = set.texture(device, 0);
    REQUIRE(&first == &again);
    REQUIRE(first.width() == 2);
    REQUIRE(device.texturesCreated == 1);
    CHECK(device.lastTextureDesc.generateMipmaps);
    CHECK(device.lastTextureDesc.mipLevels == 1);
    set.texture(device, 1);
    REQUIRE(device.texturesCreated == 2);
    set.releaseTextures();
    set.texture(device, 0);
    REQUIRE(device.texturesCreated == 3);
}

TEST_CASE("clamped tiles are created with edge clamping", "[assets][textures]") {
    TextureSet set;
    REQUIRE(set.load(sampleSet("texture-set-clamp")));
    // Each way on its own: the first tile is held at its edge across yet still tiles down.
    REQUIRE(set.entry(0).clampU);
    REQUIRE_FALSE(set.entry(0).clampV);
    REQUIRE_FALSE(set.entry(1).clampU);
    test::FakeRenderDevice device;
    set.texture(device, 0);
    REQUIRE(device.lastTextureDesc.wrap == TextureWrap::ClampToEdge);
    REQUIRE(device.lastTextureDesc.wrapDown() == TextureWrap::Repeat);
    set.texture(device, 1);
    REQUIRE(device.lastTextureDesc.wrap == TextureWrap::Repeat);
    REQUIRE(device.lastTextureDesc.wrapDown() == TextureWrap::Repeat);
    // A part pinned to the edge a tile is held at samples that edge alone.
    CHECK(set.entry(0).holdsEdge(EdgePin{.across = true}));
    CHECK_FALSE(set.entry(0).holdsEdge(EdgePin{.down = true}));
    CHECK_FALSE(set.entry(1).holdsEdge(EdgePin{.across = true, .down = true}));
    // A description that names one way wraps the same both ways.
    REQUIRE(TextureDesc{2, 2, TextureFilter::Linear, TextureWrap::ClampToEdge}.wrapDown() ==
            TextureWrap::ClampToEdge);
}

TEST_CASE("missing images surface as file errors", "[assets][textures]") {
    const auto dir = sampleSet("texture-set-missing");
    std::filesystem::remove(dir / "textures/001_GLOW_+1.png");
    TextureSet set;
    REQUIRE(set.load(dir));
    REQUIRE_THROWS_AS(set.image(1), FileError);
}

TEST_CASE("a slot the archive keeps no picture for is clear", "[assets][textures]") {
    const auto dir = test::scratchDirectory("texture-set-no-picture");
    writeTextFile(dir / "textures.json", R"({"bitmaps": [
  {"index": 0, "name": "HANDGLOW", "file": "textures/000_HANDGLOW.png", "width": 32,
   "height": 32, "flags": 429, "frames": 5}]})");
    TextureSet set;
    REQUIRE(set.load(dir));
    REQUIRE(set.entry(0).noPicture);
    const Image& clear = set.image(0); // no such file, and none is looked for
    REQUIRE(clear.width == 1);
    REQUIRE(clear.height == 1);
    REQUIRE(clear.pixels == std::vector<u8>{0, 0, 0, 0});
    test::FakeRenderDevice device;
    REQUIRE(set.texture(device, 0).width() == 1);
    CHECK_FALSE(device.lastTextureDesc.generateMipmaps);
}

TEST_CASE("a missing or malformed manifest fails to load", "[assets][textures]") {
    TextureSet set;
    REQUIRE_FALSE(set.load(test::scratchDirectory("texture-set-empty")));
    REQUIRE_FALSE(set.loaded());

    const auto dir = test::scratchDirectory("texture-set-bad");
    writeTextFile(dir / "textures.json", R"({ "bitmaps": [ { "name": 3 } ] )");
    REQUIRE_FALSE(set.load(dir));
    REQUIRE(set.size() == 0);
}

TEST_CASE("the native title set names the backdrop tiles", "[assets][textures]") {
    const auto dir = test::assetOrSkip("TITLE/textures.ngc").parent_path();
    TextureSet set;
    REQUIRE(set.load(dir));
    REQUIRE(set.size() == 16);
    REQUIRE(set.find("TITLE00") == 11U);
    REQUIRE(set.entry(0).frames == 10);
    REQUIRE(set.entry(0).halfResolution);
}

TEST_CASE("native textures generate mips instead of reading legacy metadata as pixels",
          "[assets][textures][mipmaps]") {
    u32 legacyMipCounts = 0;
    for (const auto* name : {"LEVELS/LEVELG1/objects.ngc", "LEVELS/LEVELE1/objects.ngc",
                             "MONSTERS/PLA/objects.ngc", "TITLE/objects.ngc"}) {
        const auto objects = test::assetOrSkip(name);
        const auto archive = formats::ModelArchive::parse(readFile(objects));
        TextureSet set;
        REQUIRE(set.load(objects.parent_path()));
        test::FakeRenderDevice device;
        for (u32 index = 0; index < set.size(); ++index) {
            const auto& entry = set.entry(index);
            if (entry.noPicture || entry.external()) {
                continue;
            }
            CAPTURE(name, index, entry.name);
            const auto& texture =
                dynamic_cast<const test::FakeTexture&>(set.texture(device, index));
            const auto& desc = device.lastTextureDesc;
            CHECK(desc.generateMipmaps);
            CHECK(desc.mipLevels == 1);
            CHECK(texture.pixels == set.image(index).pixels);
            if (archive.bitmaps()[index].mipmapCount > 0) {
                ++legacyMipCounts;
            }
        }
    }
    CHECK(legacyMipCounts > 0);
}

TEST_CASE("the last Plague acid frame ends exactly at the texture file boundary",
          "[assets][textures][mipmaps]") {
    const auto objects = test::assetOrSkip("MONSTERS/PLA/objects.ngc");
    const auto archive = formats::ModelArchive::parse(readFile(objects));
    const auto pixels = readFile(test::assetOrSkip("MONSTERS/PLA/textures.ngc"));
    const auto& bitmap = archive.bitmaps().at(145);
    REQUIRE(bitmap.mipmapCount == 1);
    CHECK(bitmap.dataOffset + formats::bitmapDataSize(bitmap) == pixels.size());
    CHECK_NOTHROW(formats::decodeGcTexture(bitmap, pixels));
}

} // namespace

TEST_CASE("an indexed texture whose palette shows nothing keeps its silhouette",
          "[assets][textures][silhouette]") {
    const auto dir = test::scratchDirectory("texture-set-silhouette");
    std::filesystem::create_directories(dir / "textures");
    writeFile(dir / "textures/000_LAVA.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({"defs": [], "bitmaps": [
  {"index": 0, "name": "LAVA", "file": "textures/000_LAVA.png", "width": 8, "height": 8, "flags": 0},
  {"index": 1, "name": "", "width": 8, "height": 8, "flags": 140,
   "silhouette": ["00000000", "00111100", "00111100", "00111100",
                  "00111100", "00111100", "00111100", "00000000"]}]})");
    test::convertModelFixture(dir);
    TextureSet set;
    REQUIRE(set.load(dir));
    REQUIRE(set.size() == 2);
    CHECK(set.silhouette(0) == nullptr);
    const Image* shape = set.silhouette(1);
    REQUIRE(shape != nullptr);
    CHECK(shape->width == 8);
    CHECK(shape->pixel(0, 0).a == 0);
    CHECK(shape->pixel(3, 3) == Color::white());
    CHECK(shape->pixel(7, 4).a == 0);
    // The picture itself draws nothing, which is why the shape is worth keeping.
    const Image& blank = set.image(1);
    CHECK(std::ranges::all_of(blank.pixels, [](u8 byte) { return byte == 0; }));
    test::FakeRenderDevice device;
    CHECK(set.silhouetteTexture(device, 0) == nullptr);
    const Texture* texture = set.silhouetteTexture(device, 1);
    REQUIRE(texture != nullptr);
    CHECK(device.lastTextureDesc.wrap == TextureWrap::ClampToEdge);
    CHECK(set.silhouetteTexture(device, 1) == texture);
}
