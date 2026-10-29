#include <array>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/AssetLocator.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/config/GameConfig.h"
#include "game/level/LevelWorld.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

class ImportedGameplay final : public ImportedLevel {
public:
    s32 resets = 0;
    s32 follows = 0;
    s32 partyUpdates = 0;
    s32 animationUpdates = 0;
    f64 animationSeconds = 0;
    s32 grants = 0;
    bool block = false;
    bool travel = false;
    bool activate = true;
    usize visitors = 0;
    WorldCamera native;
    Vec3 attention{0};
    std::vector<CameraSubject> subjects;
    std::vector<ImportedPickup> pickups;
    std::vector<ImportedHazardHit> hazards;
    s32 watches = 0;
    s32 soundServices = 0;
    s32 soundStops = 0;
    s32 soundPauses = 0;
    mutable std::vector<f32> samples;
    mutable std::vector<WorldCamera> drawnCameras;
    ImportedGameplay() {
        native.position = {0, 15, -20};
        native.yaw = kPi / 2;
        native.pitch = .6f;
    }
    void capturePresentation() override {}
    void update(f32 seconds, bool /*timeStopped*/) override {
        ++animationUpdates;
        animationSeconds += seconds;
    }
    void draw(RenderDevice& /*device*/, const Mat4& /*clip*/, const WorldCamera& view,
              TreeModel::Pass /*pass*/, f32 /*presentationAlpha*/) const override {
        drawnCameras.push_back(view);
    }
    void resetCamera(std::span<const CameraSubject> input) override {
        ++resets;
        subjects.assign(input.begin(), input.end());
    }
    void followCamera(std::span<const CameraSubject> input, f32 /*seconds*/) override {
        ++follows;
        subjects.assign(input.begin(), input.end());
    }
    std::optional<WorldCamera> camera(f32 alpha) const override {
        samples.push_back(alpha);
        auto result = native;
        if (alpha >= 0) {
            result.position.x = alpha * 10;
        }
        return result;
    }
    std::optional<Vec3> cameraAttention() const override { return attention; }
    Vec3 resolveMovement(const Vec3& from, const Vec3& to, f32 /*radius*/,
                         f32 /*height*/) const override {
        return block && to.x > from.x ? Vec3{from.x, to.y, to.z} : to;
    }
    void updateParty(std::span<PlayerActor*> actors, f32 /*seconds*/, bool activateItems) override {
        ++partyUpdates;
        activate = activateItems;
        visitors = actors.size();
        if (grants == 0 && !actors.empty()) {
            const ItemOffer offer{.kind = static_cast<s32>(ItemKind::Gold), .amount = 17};
            const auto taking = takeItem(actors.front()->save(), offer);
            pickups.push_back({actors.front()->player(), offer, "Gold", taking});
            ++grants;
        }
    }
    std::vector<ImportedPickup> takePickups() override { return std::exchange(pickups, {}); }
    std::vector<ImportedHazardHit> takeHazardHits() override { return std::exchange(hazards, {}); }
    void watch(const ViewVolume& /*view*/) override { ++watches; }
    void playSounds(SoundPlayer& /*player*/, const Vec3& /*listener*/) override { ++soundServices; }
    void pauseSounds() override { ++soundPauses; }
    void stopSounds() override { ++soundStops; }
    bool travelRequested() const override { return travel; }
};

TEST_CASE("Imported maps retain ordinary gameplay while supplying native camera and contacts",
          "[legends][imported-play][assets]") {
    const auto root = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    const AssetLocator assets(root);
    GameConfig config;
    config.combat.autoActivateItems = false;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
    test::FakeRenderDevice device;
    LevelWorld world;
    auto imported = std::make_shared<ImportedGameplay>();
    LevelRef ref;
    ref.realm = "Legends";
    ref.name = "n64:test";
    WorldCollision collision;
    collision.build(
        {{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, -100}}, 0, 4},
         {{0, 1, 0}, {Vec3{100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, 100}}, 0, 4}});
    auto layout = WorldLayout::fromDecoded(
        {}, {{.kind = LocatorKind::Start, .position = {0, 0, 0}}}, {-100, 0, -100}, {100, 40, 100});
    const bool loaded =
        world.loadImported(device, root, ref, std::move(layout), std::move(collision), imported);
    REQUIRE(loaded);
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.assets = &assets;
    context.unpackedRoot = root;
    const std::array party{PartyMember{0, CharacterSave{}}};
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{0};
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    CHECK(imported->resets == 1);
    REQUIRE(scene.actor(0));
    REQUIRE(scene.animator(0));
    CHECK(scene.animator(0)->bound());
    CHECK(scene.weaponHeld(0));
    for (s32 tick = 0; tick < 180; ++tick) {
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
    }
    REQUIRE_FALSE(scene.spawning());
    REQUIRE_FALSE(scene.animator(0)->entering());
    CHECK(imported->partyUpdates > 0);
    CHECK(imported->visitors == 1);
    CHECK(imported->watches > 0);
    CHECK_FALSE(imported->activate);
    CHECK(scene.actor(0)->save().gold == 17);
    CHECK(scene.viewCamera().yaw == imported->native.yaw);
    CHECK(scene.viewCamera().position == imported->native.position);

    SECTION("pausing suspends native motion sounds without closing the native scene") {
        const auto stopped = imported->soundStops;
        const auto paused = imported->soundPauses;
        const auto animations = imported->animationUpdates;
        const Vec3 position = scene.actor(0)->position();
        scene.pauseGameplaySounds();
        CHECK(imported->soundPauses == paused + 1);
        CHECK(imported->soundStops == stopped);
        CHECK(imported->animationUpdates == animations);
        CHECK(scene.actor(0)->position() == position);
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(imported->animationUpdates > animations);
    }
    SECTION("movement and imported body contacts use the native camera yaw") {
        PlayScene::Inputs inputs{};
        inputs[0].move = {{0, 1}, 1};
        const Vec3 initial = scene.actor(0)->position();
        for (s32 tick = 0; tick < 10; ++tick) {
            REQUIRE(scene.update(1.0 / 30, inputs) == PlayOutcome::Running);
        }
        const Vec3 moved = scene.actor(0)->position();
        CHECK(moved.x > initial.x + .5f);
        CHECK(moved.z == Catch::Approx(initial.z).margin(.001f));
        imported->block = true;
        for (s32 tick = 0; tick < 10; ++tick) {
            REQUIRE(scene.update(1.0 / 30, inputs) == PlayOutcome::Running);
        }
        CHECK(scene.actor(0)->position().x == Catch::Approx(moved.x).margin(.001f));
        REQUIRE(imported->subjects.size() == 1);
        CHECK(imported->subjects.front().feet == scene.actor(0)->position());
        REQUIRE(scene.relocate({3, 0, 4}));
        CHECK(imported->resets == 2);
    }
    SECTION("ordinary weapon attacks and HUD render without a preview-only control path") {
        for (s32 tick = 0; tick < 90 && scene.missiles().count() == 0; ++tick) {
            PlayScene::Inputs inputs{};
            inputs[0].attack = true;
            inputs[0].attackPressed = tick == 0;
            REQUIRE(scene.update(1.0 / 30, inputs) == PlayOutcome::Running);
        }
        REQUIRE(scene.missiles().count() > 0);
        CHECK(scene.missiles().missile(0).damage > 0);
        CHECK(scene.hudVisible());
        device.draws.clear();
        imported->drawnCameras.clear();
        scene.render(device, makeScreenProjection(640, 448), 640, 448, false, .25f);
        CHECK_FALSE(device.draws.empty());
        REQUIRE_FALSE(imported->drawnCameras.empty());
        // The callback's alpha sample is final, not interpolated a second time.
        CHECK(imported->drawnCameras.front().position.x == 2.5f);
    }
    SECTION("shared view limits recover toward the imported camera's own attention") {
        const std::array pair{PartyMember{0, CharacterSave{}}, PartyMember{1, CharacterSave{}}};
        // Pitch contributes to view depth: put both bodies behind the actual
        // view plane, not merely west of the camera's X coordinate.
        imported->native.position = {50, 15, 0};
        imported->attention = {-100, 0, 0};
        REQUIRE(scene.open(device, context, world, pair, options));
        for (s32 tick = 0; tick < 180; ++tick) {
            REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        }
        // Keep the second body's collision on the blocked side, leaving the
        // recovery direction clear instead of walking into the other player.
        REQUIRE(scene.relocate({0, 0, 0}));
        const Vec3 before = scene.actor(0)->position();
        REQUIRE(glm::dot(before - imported->native.position, imported->native.forward()) < 0);
        REQUIRE(scene.actor(1)->position().x > before.x);
        PlayScene::Inputs inputs{};
        inputs[0].move = {{0, 1}, 1};
        for (s32 tick = 0; tick < 10; ++tick) {
            REQUIRE(scene.update(1.0 / 30, inputs) == PlayOutcome::Running);
        }
        CHECK(scene.actor(0)->position().x == Catch::Approx(before.x).margin(.001f));
        inputs[0].move = {{0, -1}, 1};
        for (s32 tick = 0; tick < 10; ++tick) {
            REQUIRE(scene.update(1.0 / 30, inputs) == PlayOutcome::Running);
        }
        CHECK(scene.actor(0)->position().x < before.x - .5f);
    }
    SECTION("native travel waits for the shared departure and covering transition") {
        const s32 before = imported->partyUpdates;
        imported->travel = true;
        CHECK(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(scene.destination().realmId == -1);
        CHECK(scene.destination().name.empty());
        const s32 animationStart = imported->animationUpdates;
        const f64 secondsStart = imported->animationSeconds;
        s32 departureTicks = 0;
        PlayOutcome outcome = PlayOutcome::Running;
        for (s32 tick = 0; tick < 600 && outcome == PlayOutcome::Running; ++tick) {
            outcome = scene.update(1.0 / 30, {});
            ++departureTicks;
            // The departure branch already advances LevelWorld exactly once.
            // Native fire/portal artwork keeps moving, while pickup and portal
            // occupancy gameplay is held until the replacement scene opens.
            CHECK(imported->animationUpdates == animationStart + departureTicks);
            CHECK(imported->partyUpdates == before + 1);
        }
        CHECK(outcome == PlayOutcome::Travel);
        CHECK(departureTicks > 1);
        CHECK(imported->animationSeconds - secondsStart ==
              Catch::Approx(static_cast<f64>(departureTicks) * static_cast<f32>(1.0 / 30)));
        CHECK(imported->partyUpdates == before + 1);
    }
    SECTION("native pickup receipts target input seats without granting their rewards again") {
        const std::array thirdSeat{PartyMember{2, CharacterSave{}}};
        REQUIRE(scene.open(device, context, world, thirdSeat, options));
        for (s32 tick = 0; tick < 180; ++tick) {
            REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        }
        auto* save = scene.liveSave(2);
        REQUIRE(save);
        const ItemOffer offer{.kind = static_cast<s32>(ItemKind::Gold), .amount = 23};
        const auto taking = takeItem(*save, offer);
        REQUIRE(taking.took());
        imported->pickups.push_back({2, offer, "Treasure", taking});
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(scene.actor(2)->save().gold == 23);
        REQUIRE(scene.hud().pickups().cards().size() == 1);
        CHECK(scene.hud().pickups().cards().front().player == 2);
        CHECK(scene.hud().pickups().cards().front().texture == "GOLD");
        CHECK(imported->pickups.empty());
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(scene.actor(2)->save().gold == 23);
        CHECK(scene.hud().pickups().cards().size() == 1);
    }
    SECTION("native hazard contacts use player seats and the common health dispatcher") {
        const std::array thirdSeat{PartyMember{2, CharacterSave{}}};
        REQUIRE(scene.open(device, context, world, thirdSeat, options));
        for (s32 tick = 0; tick < 180; ++tick) {
            REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        }
        const auto before = scene.liveSave(2)->health();
        imported->hazards.push_back({2, 30, true, false, {}});
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(scene.liveSave(2)->health() < before);
        CHECK(imported->hazards.empty());
        const auto after = scene.liveSave(2)->health();
        imported->hazards.push_back({0, 30, true, false, {}});
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(scene.liveSave(2)->health() == after);
    }
    SECTION("parking a scene leaves the next scene's effects audible without moving actors") {
        scene.close();
        AudioMixer mixer(48000);
        SoundPlayer sounds(mixer);
        context.sounds = &sounds;
        REQUIRE(scene.open(device, context, world, party, options));
        const CharacterSave before = scene.actor(0)->save();
        const Vec3 position = scene.actor(0)->position();
        const f32 yaw = scene.actor(0)->yaw();
        scene.holdSounds();
        scene.suspendForImportedLevel();
        SoundClip clip;
        clip.sampleRate = 48000;
        clip.channels = 1;
        clip.samples.assign(4096, .25f);
        SoundSequence nextSceneCue;
        nextSceneCue.steps.push_back({&clip, false, false});
        REQUIRE(sounds.play(nextSceneCue) != kNoSound);
        std::array<f32, 1024> mixed{};
        mixer.mix(mixed);
        CHECK(mixed.back() == Catch::Approx(.25f));
        scene.resumeAfterImportedLevel();
        CHECK(scene.actor(0)->save() == before);
        CHECK(scene.actor(0)->position() == position);
        CHECK(scene.actor(0)->yaw() == yaw);
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(imported->soundServices > 0);
        const auto stops = imported->soundStops;
        scene.close(); // before this section's SoundPlayer and borrowed clip die
        CHECK(imported->soundStops == stops + 1);
    }
    scene.close();
}
} // namespace
