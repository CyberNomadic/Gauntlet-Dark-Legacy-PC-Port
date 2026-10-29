#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <memory>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/effects/EnemyFeedbackEmission.h"
#include "game/enemies/LegendsGenerators.h"
#include "game/enemies/LevelOpponents.h"
#include "game/scenes/LegendsSceneAdapter.h"
#include "game/scenes/LegendsTraversal.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

void be16(std::vector<u8>& bytes, usize at, u16 value) {
    bytes.at(at) = static_cast<u8>(value >> 8U);
    bytes.at(at + 1) = static_cast<u8>(value);
}
void be32(std::vector<u8>& bytes, usize at, u32 value) {
    be16(bytes, at, static_cast<u16>(value >> 16U));
    be16(bytes, at + 2, static_cast<u16>(value));
}
void real(std::vector<u8>& bytes, usize at, f32 value) {
    be32(bytes, at, std::bit_cast<u32>(value));
}

std::vector<u8> nativeTemplates() {
    std::vector<u8> bytes(4 + 8 * 56);
    be32(bytes, 0, 8);
    for (usize i = 0; i < 8; ++i) {
        const usize at = 4 + i * 56;
        const u8 tier = static_cast<u8>(i % 4);
        be16(bytes, at, i < 4 ? 0x0605 : 0x0613);
        bytes[at + 8] = 18;
        bytes[at + 9] = 2;
        bytes[at + 10] = tier;
        bytes[at + 11] = tier == 0 ? 0xff : static_cast<u8>(tier - 1);
        const std::string model = std::string(i < 4 ? "GGR" : "GIG") + char('0' + tier);
        std::ranges::copy(model, bytes.begin() + static_cast<std::ptrdiff_t>(at + 12));
        be32(bytes, at + 28, 1);
        real(bytes, at + 32, 2.5f);
        real(bytes, at + 36, 6);
    }
    return bytes;
}

formats::LegendsN64Fixture nativeFixture() {
    formats::LegendsN64Fixture fixture;
    fixture.kind = formats::LegendsN64FixtureKind::Generator;
    fixture.nativeIndex = 71;
    fixture.templateKey = 0x0605;
    fixture.generatorTier = 3;
    fixture.minimumPlayers = 2;
    fixture.position = {4, 2, 7};
    fixture.facingRadians = .75f;
    return fixture;
}

TEST_CASE("N64 generators resolve their native level species and damage artwork",
          "[legends][legends-generators]") {
    auto bytes = nativeTemplates();
    auto fixture = nativeFixture();
    std::string warning;
    const auto ice = legendsN64Generator(fixture, bytes, "ICE1", {}, &warning);
    REQUIRE(ice);
    CHECK(warning.empty());
    CHECK(ice->gameplay.nativeIndex == 71);
    CHECK(ice->gameplay.kind == 16);
    CHECK(ice->gameplay.tier == 3);
    CHECK(ice->gameplay.minimumPlayers == 2);
    CHECK(ice->gameplay.position == fixture.position);
    CHECK(ice->gameplay.yaw == fixture.facingRadians);
    CHECK(ice->gameplay.body.radius == 2.5f);
    CHECK(ice->gameplay.body.height == 6);
    CHECK(ice->gameplay.body.hitPoints == 100);
    CHECK(ice->sequences == std::array<std::string, 4>{"GENERATOR:GIG0", "GENERATOR:GIG1",
                                                       "GENERATOR:GIG2", "GENERATOR:GIG3"});
    const auto castle = legendsN64Generator(fixture, bytes, "castle1", {});
    REQUIRE(castle);
    CHECK(castle->gameplay.kind == kGruntKind);
    CHECK(castle->sequences[3] == "GENERATOR:GGR3");
    CHECK_FALSE(legendsN64Generator(fixture, bytes, "unknown", {}, &warning));
    CHECK_FALSE(warning.empty());
    bytes[4 + 3 * 56 + 9] = 10;
    CHECK_FALSE(legendsN64Generator(fixture, bytes, "boss1", {}, &warning));
    CHECK(warning.find("special") != std::string::npos);
}

TEST_CASE("N64 generator adapters reject malformed and unsupported native records",
          "[legends][legends-generators]") {
    auto bytes = nativeTemplates();
    auto fixture = nativeFixture();
    std::string warning;
    SECTION("truncated table") {
        bytes.pop_back();
        CHECK_THROWS_AS(legendsN64Generator(fixture, bytes, "ice1", {}), FormatError);
    }
    SECTION("missing native identity") {
        fixture.templateKey = 0x06fe;
        CHECK_THROWS_AS(legendsN64Generator(fixture, bytes, "ice1", {}), FormatError);
    }
    SECTION("nonlinear damage states") {
        bytes[4 + 7 * 56 + 11] = 1;
        CHECK_FALSE(legendsN64Generator(fixture, bytes, "ice1", {}, &warning));
        CHECK(warning.find("non-linear") != std::string::npos);
    }
    SECTION("non-finite contact") {
        real(bytes, 4 + 7 * 56 + 32, std::numeric_limits<f32>::quiet_NaN());
        CHECK_FALSE(legendsN64Generator(fixture, bytes, "ice1", {}, &warning));
        CHECK(warning.find("contact") != std::string::npos);
    }
    SECTION("actor placement is not a generator") {
        fixture.actorInsteadOfGenerator = true;
        CHECK_FALSE(legendsN64Generator(fixture, bytes, "ice1", {}, &warning));
        CHECK(warning.find("placed-enemy") != std::string::npos);
    }
    SECTION("already destroyed") {
        fixture.generatorTier = 0;
        CHECK_FALSE(legendsN64Generator(fixture, bytes, "ice1", {}, &warning));
        CHECK(warning.empty());
    }
    SECTION("outside four participants") {
        fixture.minimumPlayers = 5;
        CHECK_FALSE(legendsN64Generator(fixture, bytes, "ice1", {}, &warning));
        CHECK_FALSE(warning.empty());
    }
}

TEST_CASE("Native generator artwork follows common damage without runtime resource creation",
          "[legends][legends-generators][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/mount1";
    if (!std::filesystem::is_directory(directory)) {
        SKIP("native N64 extraction is not installed");
    }
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                             readFile(directory / "objects.wad"));
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    REQUIRE(adapter->generators().size() >= 5);
    for (const auto& warning : adapter->warnings()) {
        INFO(warning);
        CHECK_FALSE(warning.starts_with("Native generator damage artwork"));
    }
    const auto draw = [&] {
        device.draws.clear();
        adapter->draw(device, Mat4{1}, CameraFrame::of(WorldCamera{}), TreeModel::Pass::All);
        return device.draws;
    };
    const auto same = [](const auto& left, const auto& right) {
        return std::ranges::equal(left, right, [](const auto& a, const auto& b) {
            return a.texture == b.texture && a.vertices == b.vertices && a.transform == b.transform;
        });
    };
    const auto survey = draw();
    REQUIRE_FALSE(survey.empty());
    std::vector<ImportedGeneratorState> states;
    for (const auto& generator : adapter->generators()) {
        REQUIRE(generator.kind == 1);
        states.push_back({generator.nativeIndex, -1, 0, {}, false});
    }
    adapter->updateGenerators(states);
    CHECK(same(draw(), survey)); // failed installs must not remove the native figure
    for (usize i = 0; i < states.size(); ++i) {
        const auto& generator = adapter->generators()[i];
        states[i] = {generator.nativeIndex, static_cast<s32>(i), generator.tier, generator.position,
                     true};
    }
    adapter->updateGenerators(states);
    const auto healthy = draw();
    const auto textures = device.texturesCreated;
    const auto fixtureCount = adapter->fixtureCount();
    bool changedTier = false;
    for (auto& state : states) {
        if (state.state > 1) {
            --state.state;
            changedTier = true;
        }
    }
    REQUIRE(changedTier);
    adapter->updateGenerators(states);
    const auto damaged = draw();
    CHECK_FALSE(same(damaged, healthy));
    for (auto& state : states) {
        state.state = 0;
        state.standing = false;
    }
    adapter->updateGenerators(states);
    CHECK_FALSE(same(draw(), damaged));
    CHECK(device.texturesCreated == textures);
    CHECK(adapter->fixtureCount() == fixtureCount);
    const auto destroyed = draw();
    adapter->updateGenerators(states);
    CHECK(same(draw(), destroyed)); // repeated sync never restarts a destruction pose
}

class GeneratorLevel final : public ImportedLevel {
public:
    std::vector<ImportedGenerator> roster;
    void capturePresentation() override {}
    void update(f32, bool) override {}
    void draw(RenderDevice&, const Mat4&, const WorldCamera&, TreeModel::Pass, f32) const override {
    }
    std::span<const ImportedGenerator> importedGenerators() const override { return roster; }
};

WorldCollision floorCollision() {
    WorldCollision collision;
    CollisionTriangle floor;
    floor.normal = {0, 1, 0};
    floor.vertices = {{{-100, 0, -100}, {0, 0, 100}, {100, 0, -100}}};
    floor.objectFlags = 4;
    collision.build({floor});
    return collision;
}

TEST_CASE("Imported native generators breed attackable common enemies and award destruction",
          "[legends][legends-generators][assets]") {
    const auto objects = test::assetOrSkip("MONSTERS/GRU/objects.ngc");
    const auto root = objects.parent_path().parent_path().parent_path();
    auto definition = legendsN64Generator(nativeFixture(), nativeTemplates(), "castle1", {});
    REQUIRE(definition);
    definition->gameplay.position = {0, 0, 0};
    definition->gameplay.minimumPlayers = 1;
    auto imported = std::make_shared<GeneratorLevel>();
    imported->roster.push_back(definition->gameplay);
    auto invalid = definition->gameplay;
    invalid.nativeIndex = 72;
    invalid.kind = -99;
    imported->roster.push_back(invalid);
    test::FakeRenderDevice device;
    LevelWorld world;
    LevelRef ref;
    ref.realmId = -1;
    ref.name = "n64:generator-test";
    REQUIRE(world.loadImported(device, root, ref,
                               WorldLayout::fromDecoded({}, {}, {-100, 0, -100}, {100, 20, 100}),
                               floorCollision(), imported));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> party;
    opponents.open({device, world, weapons, effects, audio, root}, party);
    CHECK(world.level() == nullptr);
    REQUIRE(opponents.levelNow() != nullptr);
    CHECK(opponents.levelNow()->bossType == -1);
    const auto& feedbackRoster = opponents.levelNow()->enemies;
    REQUIRE(feedbackRoster.size() == 1);
    CHECK(feedbackRoster[0].kind == kGruntKind);
    for (bool killed : {false, true}) {
        EnemyFeedback feedback;
        feedback.kind = kGruntKind;
        feedback.tier = 3;
        feedback.killed = killed;
        const auto sound = feedback.sound(feedbackRoster);
        REQUIRE_FALSE(sound.empty());
        CHECK(audio.hasNamed(sound));
    }
    auto states = opponents.importedGeneratorStates();
    REQUIRE(states.size() == 2);
    REQUIRE(states[0].pool == 0);
    CHECK(states[0].nativeIndex == 71);
    CHECK(states[0].standing);
    CHECK(states[0].state == 3);
    CHECK(states[1].pool == -1);
    auto& generators = opponents.generators();
    auto& enemies = opponents.enemies();
    REQUIRE(generators.count() == 1);
    CHECK(generators.externallyPresented(0));
    CHECK(generators.presentation().empty());
    generators.draw(device, Mat4{1}, WorldLighting{});
    CHECK(device.draws.empty());
    REQUIRE(generators.obstacles().size() == 1);
    CHECK(generators.struckBy({0, 2, -8}, {0, 2, 8}, 1) == 0);
    const std::array views{EnemyView{.player = 0, .position = {0, 0, 20}}};
    generators.update(2, enemies, views, {}, true);
    CHECK(enemies.count() == 0);
    generators.update(2, enemies, views);
    REQUIRE(enemies.count() == 1);
    REQUIRE(enemies.alive(0));
    CHECK(enemies.kindOf(0) == kGruntKind);
    CHECK(enemies.generatorOf(0) == 0);
    CHECK(generators.livingOf(0) == 1);
    CHECK_FALSE(enemies.targets().empty());
    REQUIRE(world.collision().floorAt(enemies.positionOf(0), 1, 1));
    enemies.draw(device, Mat4{1}, WorldLighting{});
    CHECK_FALSE(device.draws.empty());
    opponents.strikeEnemy(0, 10000, 0, {0, 0, 1}, 0, party);
    CHECK_FALSE(enemies.alive(0));
    CHECK(enemies.dying(0));
    std::string generatorSound;
    REQUIRE(opponents.strikeGenerator(
        0, 110, 0, party, [&](const GeneratorFeedbackEmission& emission) -> std::optional<u32> {
            generatorSound = emission.sound;
            return 0;
        }));
    REQUIRE_FALSE(generatorSound.empty());
    CHECK(audio.hasNamed(generatorSound));
    states = opponents.importedGeneratorStates();
    REQUIRE(states[0].state == 2);
    opponents.strikeGenerator(0, 10000, 0, party);
    states = opponents.importedGeneratorStates();
    CHECK_FALSE(states[0].standing);
    CHECK(states[0].state == 0);
    CHECK(generators.obstacles().empty());
    const s32 bred = generators.bredOf(0);
    generators.update(600, enemies, views);
    CHECK(generators.bredOf(0) == bred);
    s32 earned = 0;
    s32 destroyed = 0;
    LevelOpponents::Events events;
    events.levels = [] {};
    events.award = [&](s32 player, s32 experience, bool) {
        CHECK(player == 0);
        earned += experience;
    };
    events.destroyedGenerator = [&](s32 player) {
        CHECK(player == 0);
        ++destroyed;
    };
    opponents.settleRewards(party, events);
    CHECK(earned > 0);
    CHECK(destroyed == 1);
    opponents.close();
    CHECK(opponents.importedGeneratorStates().empty());
    CHECK_FALSE(generators.externallyPresented(0));
}

TEST_CASE("Native N64 mountain generators use real collision for common enemy births",
          "[legends][legends-generators][assets]") {
    const auto directory = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::exists(directory / "levels/mount1/ipr.rom")) {
        SKIP("native N64 Legends extraction is not installed");
    }
    const auto objects = test::assetOrSkip("MONSTERS/TRO/objects.ngc");
    const auto root = objects.parent_path().parent_path().parent_path();
    const auto generic = directory / "items/generic";
    const auto pgen = readFile(generic / "pgen.rom");
    const auto ppow = readFile(generic / "ppow.rom");
    const auto pdec = readFile(generic / "pdec.rom");
    const auto ptrap = readFile(generic / "ptrap.rom");
    const auto ptel = readFile(generic / "ptel.rom");
    const auto pcon = readFile(generic / "pcon.rom");
    const auto prnd = readFile(generic / "prnd.rom");
    const formats::LegendsN64Templates templates{ppow, pdec, pgen, ptrap, ptel, pcon, prnd};
    const auto level = directory / "levels/mount1";
    const auto fixtures = formats::decodeLegendsN64Fixtures(readFile(level / "ipr.rom"), templates);
    const auto map = formats::LegendsMap::parseN64(readFile(level / "worlds.wad"),
                                                   readFile(level / "objects.wad"));
    LegendsTraversal traversal;
    traversal.open(map, 0, CharacterSave{});
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, &traversal.collision(), Enemies::kMost, {}, 1);
    Generators generators;
    REQUIRE(generators.bind(device, WorldLayout{}, enemies, &traversal.collision(), {}, 1));
    std::vector<ImportedGenerator> installed;
    for (const auto& fixture : fixtures) {
        if (fixture.minimumPlayers > 1) {
            continue;
        }
        const auto generator = legendsN64Generator(fixture, pgen, "mount1", map.collision);
        if (!generator) {
            continue;
        }
        CHECK(generator->gameplay.kind == 1);
        REQUIRE(generators.spawnImported(device, enemies, generator->gameplay));
        installed.push_back(generator->gameplay);
    }
    REQUIRE(installed.size() >= 5);
    for (const auto& generator : installed) {
        const std::array views{
            EnemyView{.player = 0, .position = generator.position + Vec3{0, 0, 15}}};
        generators.update(2, enemies, views);
    }
    REQUIRE(enemies.count() > 0);
    for (s32 id = 0; id < Enemies::kMost; ++id) {
        if (!enemies.alive(id)) {
            continue;
        }
        CHECK(enemies.kindOf(id) == 1);
        CHECK(enemies.generatorOf(id) >= 0);
        const auto floor = traversal.collision().floorAt(enemies.positionOf(id), 1, 1);
        REQUIRE(floor);
        CHECK(enemies.positionOf(id).y == Approx(floor->y).margin(.2f));
    }
    enemies.draw(device, Mat4{1}, WorldLighting{});
    CHECK_FALSE(device.draws.empty());
}

} // namespace
