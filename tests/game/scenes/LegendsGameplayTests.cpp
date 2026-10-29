#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "formats/LegendsN64Texture.h"
#include "game/level/ImportedLevel.h"
#include "game/players/PowerupEffects.h"
#include "game/scenes/LegendsLevel.h"
#include "game/scenes/LegendsTraversal.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

class PlainImportedLevel final : public ImportedLevel {
public:
    s32 captures = 0;
    s32 follows = 0;
    s32 partyUpdates = 0;
    usize partySize = 0;
    WorldCamera view{{-30, 30, 0}, kPi / 4, kPi / 2, 0};

    void capturePresentation() override { ++captures; }
    void update(f32, bool) override {}
    void draw(RenderDevice&, const Mat4&, const WorldCamera&, TreeModel::Pass, f32) const override {
    }
    void followCamera(std::span<const CameraSubject>, f32) override { ++follows; }
    std::optional<WorldCamera> camera(f32 = -1) const override { return view; }
    void updateParty(std::span<PlayerActor*> players, f32, bool) override {
        ++partyUpdates;
        partySize = players.size();
    }
};

struct GameplayFixture {
    std::filesystem::path root = test::assetOrSkip("PLAYERS").parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    StringTable strings;
    LevelWorld world;
    PlayScene scene;

    GameplayFixture() {
        REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
        config.camera.compass = false;
        config.combat.autoMelee = false;
        config.combat.autoActivateItems = false;
    }

    void open(std::shared_ptr<ImportedLevel> imported, WorldCollision collision, const Vec3& start,
              f32 yaw, std::span<const PartyMember> party, Vec3 minimum = Vec3{-100},
              Vec3 maximum = Vec3{100}) {
        auto layout = WorldLayout::fromDecoded(
            {}, {{.kind = LocatorKind::Start, .position = start}}, minimum, maximum);
        LevelRef ref;
        ref.realmId = -1;
        ref.realm = "Legends";
        ref.name = "native-visit";
        ref.title = "Native visit";
        REQUIRE(world.loadImported(device, root, ref, std::move(layout), std::move(collision),
                                   std::move(imported)));
        GameContext context;
        context.config = &config;
        context.strings = &strings;
        context.tower = &world;
        context.unpackedRoot = root;
        PlayOptions options;
        options.position = start;
        options.yaw = yaw;
        options.welcome = false;
        REQUIRE(scene.open(device, context, world, party, options));
        for (s32 tick = 0; tick < 600 && entering(); ++tick) {
            step();
        }
        REQUIRE_FALSE(entering());
        REQUIRE_FALSE(scene.cinematicBars());
        REQUIRE(scene.canPause(party.front().player));
    }

    bool entering() const {
        return scene.spawning() ||
               std::ranges::any_of(scene.participants(), [](const auto& participant) {
                   return participant.figure && participant.figure->animator().entering();
               });
    }

    void step(const PlayScene::Inputs& input = {}) {
        REQUIRE(scene.update(1.0 / 60, input) == PlayOutcome::Running);
    }

    void fullHud() {
        device.draws.clear();
        scene.render(device, Mat4{1}, 640, 448);
        REQUIRE_FALSE(device.draws.empty());
        const auto projection =
            makeVirtualScreenTransform(Mat4{1}, static_cast<f32>(config.display.virtualWidth),
                                       static_cast<f32>(config.display.virtualHeight), 640, 448);
        std::vector<ImmediateVertex> overlay;
        for (const auto& draw : device.draws) {
            if (draw.transform == projection) {
                overlay.insert(overlay.end(), draw.vertices.begin(), draw.vertices.end());
            }
        }
        test::FakeRenderDevice expectedDevice;
        PartyHud expected;
        REQUIRE(expected.load(expectedDevice, root, &strings));
        if (scene.hud().relicsShown()) {
            expected.showRelics();
        }
        Canvas canvas;
        canvas.begin(expectedDevice, projection);
        expected.drawStatus(canvas, scene.participants());
        canvas.end();
        std::vector<ImmediateVertex> expectedVertices;
        std::array<bool, 4> plates{};
        for (const auto& draw : expectedDevice.draws) {
            expectedVertices.insert(expectedVertices.end(), draw.vertices.begin(),
                                    draw.vertices.end());
            for (const auto& vertex : draw.vertices) {
                if (vertex.position.y >= StatusBoxPainter::kY && vertex.position.x >= 0 &&
                    vertex.position.x < 4 * StatusBoxPainter::kWidth) {
                    plates[static_cast<usize>(vertex.position.x / StatusBoxPainter::kWidth)] = true;
                }
            }
        }
        REQUIRE_FALSE(expectedVertices.empty());
        CHECK(std::ranges::all_of(plates, [](bool drawn) { return drawn; }));
        // Compare the real localized four-plate painter, not a hard-coded draw count.
        // A one-plate preview or a null StringTable changes this vertex sequence.
        CHECK(std::search(overlay.begin(), overlay.end(), expectedVertices.begin(),
                          expectedVertices.end()) != overlay.end());
        REQUIRE(strings.get("select.levelShort") != "select.levelShort");
    }
};

WorldCollision plainFloor() {
    WorldCollision world;
    CollisionTriangle first;
    first.vertices = {{{-100, 0, -100}, {-100, 0, 100}, {100, 0, 100}}};
    first.objectFlags = 4;
    CollisionTriangle second;
    second.vertices = {{{-100, 0, -100}, {100, 0, 100}, {100, 0, -100}}};
    second.objectFlags = 4;
    world.build({first, second});
    return world;
}

PartyMember member(s32 player) {
    PartyMember result;
    result.player = player;
    result.slot = static_cast<usize>(player);
    result.save.name = "VISIT" + std::to_string(player);
    result.save.color = player;
    result.save.progress().health = 500;
    result.save.gold = 100;
    result.turbo = TurboMeter::kFull;
    return result;
}

TEST_CASE("Imported maps use ordinary four-player controls inventory and localized HUD",
          "[legends][gameplay][imported-level][assets]") {
    GameplayFixture fixture;
    auto imported = std::make_shared<PlainImportedLevel>();
    std::array party{member(0), member(1), member(2), member(3)};
    auto& inventory = party[0].save.progress().inventory;
    inventory.potions = {1, 4};
    inventory.addPowerup(powerup::kWeapon, powerup::kThreeWayShot, 0, -1, false);
    fixture.open(imported, plainFloor(), {0, 0, 0}, 0, party);
    auto& scene = fixture.scene;
    REQUIRE(scene.actorCount() == 4);
    REQUIRE(scene.participants().size() == 4);
    CHECK(scene.simulationCamera() == imported->view);
    CHECK(scene.viewCamera() == imported->view);
    CHECK(imported->partySize == 4);
    CHECK(imported->captures > 0);
    CHECK(imported->follows > 0);
    for (s32 player = 0; player < 4; ++player) {
        REQUIRE(scene.actor(player));
        REQUIRE(scene.animator(player));
        CHECK(scene.runtime(player)->slot == static_cast<usize>(player));
        CHECK(scene.status(player).active);
        CHECK(scene.status(player).name == party[static_cast<usize>(player)].save.name);
    }
    fixture.fullHud();

    PlayScene::Inputs attack;
    attack[0].attack = true;
    for (s32 tick = 0; tick < 90 && scene.missiles().count() == 0; ++tick) {
        fixture.step(attack);
    }
    REQUIRE(scene.missiles().count() == 1);
    CHECK(scene.missiles().missile(0).potion == 0);
    for (s32 tick = 0; tick < 300 && scene.missiles().count() != 0; ++tick) {
        fixture.step();
    }

    PlayScene::Inputs select;
    select[0].selector.up = true;
    fixture.step(select);
    for (s32 tick = 0; tick < 90 && !scene.selector(0).showing(); ++tick) {
        fixture.step();
    }
    REQUIRE(scene.selector(0).showing());
    fixture.step(select);
    CHECK(PowerupEffects::of(scene.actor(0)->save().progress().inventory).shots() == 3);
    for (s32 tick = 0; tick < 90 && scene.missiles().count() == 0; ++tick) {
        fixture.step(attack);
    }
    REQUIRE(scene.missiles().count() == 3);
    for (s32 tick = 0; tick < 180; ++tick) {
        fixture.step();
    }

    PlayScene::Inputs potion;
    potion[0].usePotion = true;
    for (s32 tick = 0; tick < 90 && scene.actor(0)->save().progress().inventory.potions.size() == 2;
         ++tick) {
        fixture.step(potion);
    }
    CHECK(scene.actor(0)->save().progress().inventory.potions == std::vector<s32>{1});
    CHECK(scene.actor(1)->save().progress().inventory.potions.empty());
    CHECK(scene.effects().count() > 0);
    for (s32 tick = 0; tick < 60; ++tick) {
        fixture.step(potion);
    }
    CHECK(scene.actor(0)->save().progress().inventory.potions == std::vector<s32>{1});

    // Move a different seat. Native yaw affects actual movement, not just drawing.
    const auto before = scene.actor(3)->position();
    PlayScene::Inputs move;
    move[3].move = {{0, 1}, 1};
    for (s32 tick = 0; tick < 40; ++tick) {
        fixture.step(move);
    }
    const Vec3 delta = scene.actor(3)->position() - before;
    CHECK(delta.x > 1);
    CHECK(std::abs(delta.z) < .1f);
    CHECK(imported->partyUpdates > 0);
    CHECK_FALSE(scene.leaving());
    CHECK(scene.canPause(0));
    scene.close();
}

formats::LegendsMap nativeMap(const std::filesystem::path& directory) {
    const auto models = readFile(directory / "objects.wad");
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), models,
                                             readFile(directory / "ipr.rom"));
    for (auto& texture : formats::decodeLegendsN64Textures(models)) {
        map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                texture.hasTransparency, std::move(texture.name), texture.external,
                                texture.width, texture.height});
    }
    return map;
}

TEST_CASE("Native N64 Tower runs inside PlayScene with native camera scenery and GDL combat",
          "[legends][gameplay][tower][assets]") {
    GameplayFixture fixture;
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/tower2";
    if (!std::filesystem::is_directory(directory)) {
        SKIP("Native N64 Tower extraction is not installed");
    }
    auto map = nativeMap(directory);
    auto adapter = LegendsSceneAdapter::load(fixture.device, LegendsPlatform::N64, directory, map);
    REQUIRE(adapter);
    REQUIRE(adapter->cameraConfig());
    const auto partyMember = member(0);
    ClassDataSet classes;
    REQUIRE(classes.load(fixture.root / "pdata"));
    LegendsTraversal probe;
    probe.open(map, partyMember.player, partyMember.save,
               classes.stats(partyMember.save.character));
    REQUIRE(probe.authoredStart());
    auto imported = std::make_shared<LegendsLevel>(fixture.device, map, std::move(adapter),
                                                   directory, fixture.root);
    const std::array party{partyMember};
    fixture.open(imported, probe.collision(), probe.start(), probe.actor().yaw(), party,
                 map.minBounds, map.maxBounds);
    auto& scene = fixture.scene;
    REQUIRE(scene.world() == &fixture.world);
    REQUIRE(scene.actorCount() == 1);
    REQUIRE(scene.animator(0));
    REQUIRE(imported->camera());
    CHECK(scene.simulationCamera() == *imported->camera());
    CHECK(scene.simulationCamera().pitch == Approx(.05f));
    CHECK(scene.actor(0)->position().x == Approx(-4.5f));
    CHECK(scene.actor(0)->position().z == Approx(-15.5f));
    CHECK_FALSE(scene.status(1).active);
    CHECK_FALSE(scene.status(2).active);
    CHECK_FALSE(scene.status(3).active);
    fixture.fullHud();
    const auto worldDraws = std::ranges::count_if(fixture.device.draws, [&](const auto& draw) {
        return draw.state.depthTest && draw.texture != &fixture.device.whiteTexture() &&
               !draw.vertices.empty();
    });
    CHECK(worldDraws > 10);

    PlayScene::Inputs attack;
    attack[0].attack = true;
    bool threw = false;
    for (s32 tick = 0; tick < 90 && !threw; ++tick) {
        fixture.step(attack);
        threw = scene.missiles().count() != 0;
    }
    REQUIRE(threw);
    for (s32 tick = 0; tick < 90; ++tick) {
        fixture.step();
    }

    // Sumner's body lives in the native script rather than worlds.wad. Exercise
    // the adapter through PartyMotion, not just its standalone cylinder helper.
    REQUIRE(scene.relocate({10, 0, -8}));
    for (s32 tick = 0; tick < 90; ++tick) {
        const f32 relative = -kPi / 2 - scene.simulationCamera().yaw;
        PlayScene::Inputs move;
        move[0].move = {{std::sin(relative), std::cos(relative)}, 1};
        fixture.step(move);
    }
    const auto position = scene.actor(0)->position();
    CHECK(position.x >= 6 + 1.5f + scene.actor(0)->radius() - .02f);
    CHECK(position.x < 10);
    CHECK(position.z == Approx(-8).margin(.05f));
    CHECK(scene.simulationCamera() == *imported->camera());
    CHECK_FALSE(scene.leaving());
    CHECK(scene.canPause(0));
    scene.close();
}

TEST_CASE("Native Tower exits wait for the party then spin and shrink with native silver artwork",
          "[legends][gameplay][portal][tower][assets]") {
    GameplayFixture fixture;
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/tower2";
    if (!std::filesystem::is_directory(directory)) {
        SKIP("Native N64 Tower extraction is not installed");
    }
    auto map = nativeMap(directory);
    auto adapter =
        LegendsSceneAdapter::load(fixture.device, LegendsPlatform::N64, directory, map, 2);
    REQUIRE(adapter);
    const auto found = std::ranges::find_if(
        adapter->portals(), [](const auto& portal) { return portal.map == "mount1"; });
    REQUIRE(found != adapter->portals().end());
    const auto portal = *found;
    const std::array party{member(0), member(1)};
    LegendsTraversal probe;
    probe.open(map, 0, party[0].save);
    auto imported = std::make_shared<LegendsLevel>(fixture.device, map, std::move(adapter),
                                                   directory, fixture.root);
    fixture.open(imported, probe.collision(), probe.start(), probe.actor().yaw(), party,
                 map.minBounds, map.maxBounds);
    auto& scene = fixture.scene;
    REQUIRE(scene.noclipMove(0, portal.position - scene.actor(0)->position(), std::nullopt));
    // A fixed +X offset can lie past this wedge's outer wall. Parking there tests
    // the fall-rescue path, which correctly returns the player beside its partner
    // and can therefore enter the portal. Use clear native ground toward the hub.
    const auto* waitingActor = scene.actor(1);
    const Vec2 inward = glm::normalize(Vec2{-portal.position.x, -portal.position.z});
    Vec3 waiting = portal.position + Vec3{inward.x, 0, inward.y} * 12.0f;
    const auto floor = fixture.world.collision().floorAt(waiting, 4, 10);
    REQUIRE(floor);
    waiting.y = floor->y;
    const auto clear = fixture.world.collision().resolveWalls(
        waiting, waitingActor->radius(), waiting.y + PlayerActor::kFootClearance,
        waiting.y + waitingActor->height() - PlayerActor::kFootClearance);
    REQUIRE(glm::distance(waiting, clear) < .001f);
    REQUIRE_FALSE(touchesLegendsPortal(
        portal, {1, waiting, true, waitingActor->radius(), waitingActor->height()}));
    REQUIRE(scene.noclipMove(1, waiting - waitingActor->position(), std::nullopt));
    for (s32 tick = 0; tick < 180; ++tick) {
        fixture.step();
        CHECK_FALSE(scene.leaving());
        CHECK_FALSE(imported->travelRequested());
        CAPTURE(tick, waiting.x, waiting.y, waiting.z, scene.actor(1)->position().x,
                scene.actor(1)->position().y, scene.actor(1)->position().z);
        REQUIRE_FALSE(
            touchesLegendsPortal(portal, {1, scene.actor(1)->position(), true,
                                          scene.actor(1)->radius(), scene.actor(1)->height()}));
    }
    REQUIRE(touchesLegendsPortal(portal, {0, scene.actor(0)->position(), true,
                                          scene.actor(0)->radius(), scene.actor(0)->height()}));
    CHECK_FALSE(touchesLegendsPortal(portal, {1, scene.actor(1)->position(), true,
                                              scene.actor(1)->radius(), scene.actor(1)->height()}));
    // Join the first player without overlapping bodies. Contact is still tested
    // by the native portal through the ordinary post-movement scene callback.
    REQUIRE(scene.noclipMove(1, portal.position + Vec3{2, 0, 0} - scene.actor(1)->position(),
                             std::nullopt));
    s32 waited = 0;
    for (; waited < 240 && !scene.leaving(); ++waited) {
        fixture.step();
    }
    REQUIRE(scene.leaving());
    REQUIRE(imported->destination());
    CHECK(*imported->destination() == "mount1");
    CHECK(waited >= 120);
    CHECK_FALSE(scene.departure().started()); // The ordinary GDL sink remains inactive.
    REQUIRE(imported->departure());
    CHECK_FALSE(imported->departure()->finished);
    CHECK(imported->departure()->scale == 1);
    REQUIRE(imported->departure()->skin);
    const auto* skin = imported->departure()->skin;
    const auto sourceTextures = formats::decodeLegendsN64Textures(
        readFile(directory.parent_path().parent_path() / "static/objects.wad"));
    const auto nativeSkin = std::ranges::find(sourceTextures, std::string{"CHROMESILVER"},
                                              &formats::LegendsN64Texture::name);
    REQUIRE(nativeSkin != sourceTextures.end());
    const auto* recordedSkin = dynamic_cast<const test::FakeTexture*>(skin);
    REQUIRE(recordedSkin);
    CHECK(recordedSkin->pixels == nativeSkin->image.pixels);
    CHECK_FALSE(scene.canPause(0));
    CHECK_FALSE(scene.canPause(1));
    const auto feet0 = scene.actor(0)->position();
    const auto feet1 = scene.actor(1)->position();
    const auto departingView = imported->camera();
    REQUIRE(departingView);
    fixture.device.draws.clear();
    scene.render(fixture.device, Mat4{1}, 640, 448, false, 1);
    const auto firstBody = std::ranges::find_if(fixture.device.draws, [&](const auto& draw) {
        return draw.state.maskedTexture == skin && !draw.vertices.empty();
    });
    REQUIRE(firstBody != fixture.device.draws.end());
    // TreeModel bakes placement into submitted vertices; the draw matrix is
    // only the camera projection. Measure the body itself about its unchanged feet.
    const auto originalVertices = firstBody->vertices;
    REQUIRE(std::ranges::any_of(originalVertices, [&](const auto& vertex) {
        return glm::distance(vertex.position, feet0) > .1f;
    }));
    const auto texturesBeforeDeparture = fixture.device.texturesCreated;
    const auto* runtime = scene.runtime(0);
    REQUIRE(runtime);
    REQUIRE(runtime->figure);
    const auto beforeAnimation = runtime->figure->animator().player().checkpoint();
    const auto beforeAction = runtime->figure->animator().action();
    const auto beforeMissiles = scene.missiles().count();
    PlayScene::Inputs held;
    held[0].attack = true;
    held[0].move = {{0, 1}, 1};
    held[1].move = {{1, 0}, 1};
    for (s32 tick = 0; tick < 12; ++tick) {
        fixture.step(held);
    }
    CHECK(scene.actor(0)->position() == feet0);
    CHECK(scene.actor(1)->position() == feet1);
    CHECK(scene.departure().displacement().y == 0);
    const auto departure = imported->departure();
    REQUIRE(departure);
    CHECK(departure->yaw == Approx(9.3f * .2f));
    CHECK(departure->scale == Approx(.91f));
    CHECK_FALSE(departure->finished);
    CHECK(imported->camera(0)->position == departingView->position);
    CHECK(imported->camera(1)->position == departingView->position);
    CHECK(imported->camera(0)->yaw == departingView->yaw);
    CHECK(imported->camera(1)->pitch == departingView->pitch);
    const auto& animator = runtime->figure->animator();
    const auto afterAnimation = animator.player().checkpoint();
    CHECK(afterAnimation.sequence == beforeAnimation.sequence);
    CHECK((afterAnimation.time != beforeAnimation.time ||
           afterAnimation.generation != beforeAnimation.generation));
    CHECK(animator.action() == beforeAction);
    CHECK_FALSE(animator.released());
    CHECK_FALSE(animator.strongReleased());
    CHECK_FALSE(animator.superReleased());
    CHECK_FALSE(animator.meleeStruck());
    CHECK(animator.itemReleased() == PlayerDeed::None);
    CHECK_FALSE(runtime->figure->familiarReleased());
    CHECK(scene.missiles().count() == beforeMissiles);
    fixture.device.draws.clear();
    scene.render(fixture.device, Mat4{1}, 640, 448, false, 1);
    const auto smallerBody = std::ranges::find_if(fixture.device.draws, [&](const auto& draw) {
        return draw.state.maskedTexture == skin && !draw.vertices.empty();
    });
    REQUIRE(smallerBody != fixture.device.draws.end());
    const auto smallerVertices = smallerBody->vertices;
    const f32 renderedScale = imported->departure(1)->scale;
    // Compare against the same CURRENT animated pose at ordinary placement, not
    // against an earlier frame. The native exit does not freeze the body tree.
    fixture.device.draws.clear();
    const auto worn = PowerupEffects::of(runtime->actor.save().progress().inventory);
    const auto body = PlayerFigure::bodyPlacement(PartyFigures::presentationBody(*runtime, 1),
                                                  runtime->actor.save(), worn);
    const auto camera = CameraFrame::of(*imported->camera(1));
    runtime->figure->setSkinTexture(skin);
    runtime->figure->draw(fixture.device, Mat4{1}, body, fixture.world.lighting(), 1, false,
                          &camera, 1);
    runtime->figure->setSkinTexture(nullptr);
    const auto unscaledBody = std::ranges::find_if(fixture.device.draws, [&](const auto& draw) {
        return draw.state.maskedTexture == skin && !draw.vertices.empty();
    });
    REQUIRE(unscaledBody != fixture.device.draws.end());
    REQUIRE(smallerVertices.size() == unscaledBody->vertices.size());
    for (usize vertex = 0; vertex < smallerVertices.size(); ++vertex) {
        const Vec3 original = unscaledBody->vertices[vertex].position - feet0;
        const Vec3 smaller = smallerVertices[vertex].position - feet0;
        CHECK(glm::length(smaller) == Approx(glm::length(original) * renderedScale).margin(.001f));
        CHECK(smaller.y == Approx(original.y * renderedScale).margin(.001f));
    }
    PlayOutcome outcome = PlayOutcome::Running;
    for (s32 tick = 0; tick < 240 && outcome == PlayOutcome::Running; ++tick) {
        outcome = scene.update(1.0 / 60, held);
    }
    CHECK(outcome == PlayOutcome::Travel);
    CHECK_FALSE(scene.departure().started());
    REQUIRE(imported->departure());
    CHECK(imported->departure()->finished);
    CHECK(imported->departure()->scale == Approx(.1f));
    CHECK(fixture.device.texturesCreated == texturesBeforeDeparture);
    CHECK(scene.transition().covering());
    CHECK(*imported->destination() == "mount1");
    // Common travel intent contains no forged GameCube realm or completion.
    CHECK(scene.destination().realmId == -1);
    scene.close();
}

TEST_CASE("Native scroll pickups open the shared scroll and hold ordinary gameplay",
          "[legends][gameplay][scrolls][assets]") {
    GameplayFixture fixture;
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/mount1";
    if (!std::filesystem::is_directory(directory)) {
        SKIP("Native N64 Mountain extraction is not installed");
    }
    auto map = nativeMap(directory);
    auto adapter = LegendsSceneAdapter::load(fixture.device, LegendsPlatform::N64, directory, map);
    const auto pickup = std::ranges::find_if(adapter->pickups(), [](const auto& value) {
        return value.offer.kind == static_cast<s32>(ItemKind::Scroll) && !value.scrollPages.empty();
    });
    REQUIRE(pickup != adapter->pickups().end());
    const auto position = pickup->position;
    const auto pickupIndex = static_cast<usize>(pickup - adapter->pickups().begin());
    const std::array party{member(0)};
    LegendsTraversal probe;
    probe.open(map, 0, party[0].save);
    auto imported = std::make_shared<LegendsLevel>(fixture.device, map, std::move(adapter),
                                                   directory, fixture.root);
    fixture.open(imported, probe.collision(), probe.start(), probe.actor().yaw(), party,
                 map.minBounds, map.maxBounds);
    REQUIRE(
        fixture.scene.noclipMove(0, position - fixture.scene.actor(0)->position(), std::nullopt));
    fixture.step();
    REQUIRE(fixture.scene.scroll().active());
    REQUIRE(imported->adapter().pickups()[pickupIndex].collected);
    CHECK_FALSE(fixture.scene.scrollLook()); // No accidental GDL message identity.
    const auto heldPosition = fixture.scene.actor(0)->position();
    PlayScene::Inputs move;
    move[0].move = {{1, 0}, 1};
    for (s32 tick = 0; tick < 20; ++tick) {
        fixture.step(move);
    }
    CHECK(fixture.scene.actor(0)->position() == heldPosition);
    CHECK_FALSE(imported->travelRequested());
    fixture.scene.close();
}
} // namespace
