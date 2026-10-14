#include <array>
#include <limits>
#include <memory>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/StageStatePacket.h"
#include "game/screens/CombatCapture.h"
#include "game/screens/CombatPrediction.h"
#include "game/screens/OnlineCombatReplay.h"
#include "game/screens/SessionInputs.h"
#include "game/screens/StageParticlePresentation.h"
#include "game/screens/StageReplayPresentation.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Presenter = StageReplayPresentation;

EnemyPresentation provenance(const CombatSnapshot& state) {
    EnemyPresentation result;
    result.epoch = state.motion.epoch;
    result.tick = state.motion.tick;
    result.shownGeometry = state.geometry;
    REQUIRE(result.matches(state));
    return result;
}

std::vector<u8> bytes(const CombatSnapshot& state) {
    const auto encoded = CombatPacket::encode(state);
    REQUIRE(encoded);
    return *encoded;
}

struct Fixture {
    test::FakeRenderDevice device;
    LevelWorld world;
    EffectTrees missiles, effects;
    MatchContext context;
    std::shared_ptr<const void> lifetime = std::make_shared<const u8>(0);
    std::unique_ptr<CombatSnapshot> baseline, previous, current;
    std::shared_ptr<const StageParticlePresentation> beforeParticles, afterParticles;
    std::vector<PointLight> beforeLights{{{0, 3, 0}, {1, 0, 0}, 20, 2}};
    std::vector<PointLight> afterLights{{{2, 3, 0}, {0, 0, 1}, 30, 10}};
    Presenter::Authority identity;

    Fixture() {
        const auto directory = test::sampleLevel("stage-replay-presentation");
        writeTextFile(directory / "world.json", R"({"objects":[
          {"name":"FLOOR","position":[0,0,0],"flags":4},
          {"name":"PSYSA","position":[1,2,3],"flags":2048}],
          "particles":[{"id":"A","preset":2,"enables":16929,"texture":"STONE",
            "particleLife":[1,0],"rate":[30,30,30,30]}]})");
        REQUIRE(world.load(device, directory, {.name = "TEST", .directory = "."}));
        context.epoch = context.scene = 1;
        context.owners = {1, 2, 0, 0};
        context.grants = {1, 2, 0, 0};
        baseline = picture(10, 0);
        previous = picture(11, 0);
        current = picture(12, 2);
        const auto key = Presenter::authority(context, 2, *baseline, lifetime);
        REQUIRE(key);
        identity = *key;
        world.capturePresentation();
        world.update(1.0 / 30);
        StageParticlePresentation::Sources sources{world, missiles, effects};
        sources.lights = beforeLights;
        beforeParticles = StageParticlePresentation::capture(1, 11, lifetime, sources);
        world.capturePresentation();
        world.update(1.0 / 30);
        sources.lights = afterLights;
        afterParticles = StageParticlePresentation::capture(1, 12, lifetime, sources);
        REQUIRE(beforeParticles);
        REQUIRE(afterParticles);
        REQUIRE(afterParticles->particleCount() > 0);
    }
    std::unique_ptr<CombatSnapshot> picture(u64 tick, f32 height) const {
        auto result = std::make_unique<CombatSnapshot>();
        result->motion.epoch = result->motion.cameraContinuity = 1;
        result->motion.tick = tick;
        for (usize seat = 0; seat < 2; ++seat) {
            result->motion.players[seat] =
                SeatMotion{context.grants[seat], 1, {static_cast<f32>(seat) * 4, height, 0}, 0};
            result->players[seat] = PlayerCombatState{
                1000, ReplicaPlayerLife::Standing, false, true, {4, 1, 1, height, 1}};
        }
        result->geometry = world.scene().geometry();
        result->worldTextureFrame = height;
        REQUIRE(result->valid());
        return result;
    }
    std::shared_ptr<const Presenter::Publication> publish(u64 revision = 7) const {
        return Presenter::publish(identity, revision, *previous, *current, beforeParticles,
                                  afterParticles);
    }
};

TEST_CASE("detached Stage lighting uses immutable current endpoint lights at every blend",
          "[netplay][stage-replay-presentation][stage-point-lights]") {
    auto fixture = std::make_unique<Fixture>();
    const auto publication = fixture->publish();
    REQUIRE(publication);
    const auto expected = fixture->afterLights.front();
    fixture->beforeLights.clear();
    fixture->afterLights.front().position = {100, 200, 300};
    fixture->world.capturePresentation();
    fixture->world.update(1.0f / 30);
    Presenter presenter;
    const Presenter::Time start{std::chrono::seconds{1}};
    REQUIRE(presenter.adopt(publication, fixture->identity, start));
    for (const auto elapsed : {0, 4, 8, 17, 100}) {
        auto shown = std::make_unique<CombatSnapshot>(*fixture->baseline);
        auto presentation = provenance(*shown);
        REQUIRE(presenter.present(*shown, presentation, fixture->identity,
                                  start + std::chrono::milliseconds{elapsed}));
        REQUIRE(presentation.particles);
        const auto lights = presentation.particles->lights();
        REQUIRE(lights.size() == 1);
        CHECK(lights[0].position == expected.position);
        CHECK(lights[0].color == expected.color);
        CHECK(lights[0].radius == expected.radius);
        CHECK(lights[0].intensity == expected.intensity);
    }
    // A genuinely empty current light list replaces a prior lit endpoint; it is
    // not a missing sample and must not retain/interpolate the previous light.
    fixture->afterParticles = StageParticlePresentation::capture(
        1, 12, fixture->lifetime, {fixture->world, fixture->missiles, fixture->effects});
    REQUIRE(fixture->afterParticles);
    const auto dark = fixture->publish(8);
    REQUIRE(dark);
    REQUIRE(presenter.adopt(dark, fixture->identity, start));
    auto shown = std::make_unique<CombatSnapshot>(*fixture->baseline);
    auto presentation = provenance(*shown);
    REQUIRE(presenter.present(*shown, presentation, fixture->identity, start));
    CHECK(presentation.particles->lights().empty());
    fixture->lifetime.reset();
    CHECK_FALSE(presenter.active());
    CHECK_FALSE(presenter.present(*shown, presentation, fixture->identity, start));
    CHECK(fixture->beforeParticles->lights().empty());
}

TEST_CASE("detached stage publications own endpoints and preserve synchronous sampling",
          "[netplay][stage-replay-presentation]") {
    auto fixture = std::make_unique<Fixture>();
    const auto publication = fixture->publish();
    REQUIRE(publication);
    CHECK(publication->tick() == 12);
    CHECK(publication->authority().tick() == 10);
    const auto uploads = fixture->device.texturesCreated;
    const auto updates = fixture->device.textureUpdates;
    auto synchronous = std::make_unique<CombatSnapshot>(*fixture->baseline);
    auto expected = provenance(*synchronous);
    REQUIRE(Presenter::sample(*synchronous, expected, fixture->identity, *fixture->previous,
                              *fixture->current, fixture->beforeParticles, fixture->afterParticles,
                              .5f));
    const auto encoded = bytes(*synchronous);
    const CameraFrame camera;
    fixture->device.draws.clear();
    REQUIRE(expected.particles->draw(StageParticlePresentation::Phase::World, fixture->device,
                                     Mat4{1}, camera, .5f));
    const auto particleDraws = fixture->device.draws;
    REQUIRE_FALSE(particleDraws.empty());
    // Neither a native owner update nor mutation of the producer's endpoint
    // buffers can change an already completed publication.
    fixture->current->motion.players[1]->position.y = 40;
    fixture->previous->motion.players[1]->position.y = 20;
    fixture->world.capturePresentation();
    fixture->world.update(1.0 / 30);
    Presenter presenter;
    const Presenter::Time start{std::chrono::seconds{1}};
    REQUIRE(presenter.adopt(publication, fixture->identity, start));
    auto shown = std::make_unique<CombatSnapshot>(*fixture->baseline);
    auto presentation = provenance(*shown);
    REQUIRE(presenter.present(*shown, presentation, fixture->identity,
                              start + std::chrono::nanoseconds{8'333'333}));
    CHECK(shown->motion.players[1]->position.y == Approx(1).margin(0.00001));
    CHECK(presentation.particleBlend == Approx(.5f).margin(0.00001));
    CHECK(presentation.particles == fixture->afterParticles);
    // The nanosecond clock's nearest half-step differs by less than one float
    // sample; exact explicit-fraction sampling remains byte-for-byte identical.
    auto explicitSample = std::make_unique<CombatSnapshot>(*fixture->baseline);
    auto explicitPresentation = provenance(*explicitSample);
    fixture->previous->motion.players[1]->position.y = 0;
    fixture->current->motion.players[1]->position.y = 2;
    REQUIRE(Presenter::sample(*explicitSample, explicitPresentation, fixture->identity,
                              *fixture->previous, *fixture->current, fixture->beforeParticles,
                              fixture->afterParticles, .5f));
    CHECK((bytes(*explicitSample) == encoded));
    fixture->device.draws.clear();
    REQUIRE(explicitPresentation.particles->draw(StageParticlePresentation::Phase::World,
                                                 fixture->device, Mat4{1}, camera, .5f));
    REQUIRE(fixture->device.draws.size() == particleDraws.size());
    for (usize i = 0; i < particleDraws.size(); ++i) {
        CHECK(fixture->device.draws[i].texture == particleDraws[i].texture);
        CHECK(fixture->device.draws[i].vertices == particleDraws[i].vertices);
        CHECK(fixture->device.draws[i].transform == particleDraws[i].transform);
    }
    CHECK(fixture->device.texturesCreated == uploads);
    CHECK(fixture->device.textureUpdates == updates);
}

TEST_CASE("detached adoption clocks never cycle backward or restart on duplicate publication",
          "[netplay][stage-replay-presentation]") {
    auto fixture = std::make_unique<Fixture>();
    const auto publication = fixture->publish();
    REQUIRE(publication);
    Presenter presenter;
    const Presenter::Time start{std::chrono::seconds{1}};
    REQUIRE(presenter.adopt(publication, fixture->identity, start));
    const auto sample = [&](Presenter::Time time) {
        auto shown = std::make_unique<CombatSnapshot>(*fixture->baseline);
        auto presentation = provenance(*shown);
        REQUIRE(presenter.present(*shown, presentation, fixture->identity, time));
        return presentation.particleBlend;
    };
    CHECK(sample(start) == 0);
    CHECK(sample(start + std::chrono::milliseconds{5}) == Approx(.3f));
    CHECK(sample(start - std::chrono::seconds{10}) == Approx(.3f));
    REQUIRE(presenter.adopt(publication, fixture->identity, start + std::chrono::milliseconds{7}));
    CHECK(sample(start + std::chrono::milliseconds{10}) == Approx(.6f));
    CHECK(sample(Presenter::Time::max()) == 1);
    CHECK(sample(Presenter::Time::min()) == 1);
    const auto next = fixture->publish(8);
    REQUIRE(presenter.adopt(next, fixture->identity, start + std::chrono::seconds{1}));
    CHECK(sample(start + std::chrono::seconds{1}) == 0);
    CHECK_FALSE(presenter.adopt(publication, fixture->identity, start));
    CHECK_FALSE(presenter.active());
}

TEST_CASE("detached stage rejects stale authority and foreign resources without changing output",
          "[netplay][stage-replay-presentation]") {
    auto fixture = std::make_unique<Fixture>();
    const auto publication = fixture->publish();
    REQUIRE(publication);
    Presenter presenter;
    REQUIRE(presenter.adopt(publication, fixture->identity));
    auto changed = std::make_unique<CombatSnapshot>(*fixture->baseline);
    auto context = fixture->context;
    std::shared_ptr<const void> resource = fixture->lifetime;
    SECTION("newer authority") {
        ++changed->motion.tick;
    }
    SECTION("older authority") {
        --changed->motion.tick;
    }
    SECTION("another scene") {
        ++context.scene;
    }
    SECTION("another epoch") {
        ++context.epoch;
        ++changed->motion.epoch;
    }
    SECTION("another grant") {
        ++context.grants[1];
        ++changed->motion.players[1]->grant;
    }
    SECTION("camera cut") {
        ++changed->motion.cameraContinuity;
    }
    SECTION("player cut") {
        ++changed->motion.players[1]->continuity;
    }
    SECTION("another loaded resource owner") {
        resource = std::make_shared<const u8>(0);
    }
    const auto current = Presenter::authority(context, 2, *changed, resource);
    REQUIRE(current);
    auto shown = std::make_unique<CombatSnapshot>(*fixture->baseline);
    auto presentation = provenance(*shown);
    const auto original = bytes(*shown);
    Presenter::Refusal refusal{};
    CHECK_FALSE(
        presenter.present(*shown, presentation, *current, Presenter::Clock::now(), &refusal));
    CHECK(refusal == Presenter::Refusal::Authority);
    CHECK_FALSE(presenter.active());
    CHECK((bytes(*shown) == original));
    CHECK(presentation.matches(*shown));
    CHECK_FALSE(presentation.particles);
    CHECK_FALSE(presenter.adopt(publication, *current));
}

TEST_CASE("stage publication validates endpoints sidecar ownership and malformed authorities",
          "[netplay][stage-replay-presentation]") {
    auto fixture = std::make_unique<Fixture>();
    SECTION("malformed newest authority") {
        fixture->baseline->players[1]->health = std::numeric_limits<f32>::quiet_NaN();
        CHECK_FALSE(
            Presenter::authority(fixture->context, 2, *fixture->baseline, fixture->lifetime));
        return;
    }
    SECTION("portal transition") {
        fixture->baseline->players[1]->portalPhase = .5f;
        CHECK_FALSE(
            Presenter::authority(fixture->context, 2, *fixture->baseline, fixture->lifetime));
        return;
    }
    SECTION("foreign sidecar with identical epoch and tick") {
        const auto other = std::make_shared<const u8>(0);
        fixture->afterParticles = StageParticlePresentation::capture(
            1, 12, other, {fixture->world, fixture->missiles, fixture->effects, nullptr, {}});
        REQUIRE(fixture->afterParticles);
        REQUIRE(fixture->afterParticles->matches(1, 12));
        CHECK_FALSE(fixture->publish());
        return;
    }
    SECTION("missing previous sidecar") {
        fixture->beforeParticles.reset();
    }
    SECTION("nonconsecutive endpoints") {
        fixture->current->motion.tick = 13;
    }
    SECTION("foreign current seat") {
        ++fixture->current->motion.players[1]->grant;
    }
    SECTION("malformed current endpoint") {
        fixture->current->motion.players[1]->position.x = std::numeric_limits<f32>::infinity();
    }
    CHECK_FALSE(fixture->publish());
}

TEST_CASE("resource invalidation withdraws detached pictures without retaining the marker",
          "[netplay][stage-replay-presentation]") {
    auto fixture = std::make_unique<Fixture>();
    const auto publication = fixture->publish();
    REQUIRE(publication);
    Presenter presenter;
    REQUIRE(presenter.adopt(publication, fixture->identity));
    CHECK(fixture->lifetime.use_count() == 1);
    fixture->lifetime.reset(); // no native draw or teardown runs concurrently
    CHECK_FALSE(presenter.active());
    auto shown = std::make_unique<CombatSnapshot>(*fixture->baseline);
    auto presentation = provenance(*shown);
    const auto original = bytes(*shown);
    CHECK_FALSE(presenter.present(*shown, presentation, fixture->identity));
    CHECK((bytes(*shown) == original));
    CHECK_FALSE(presentation.particles);
}

TEST_CASE("completed native Stage publication survives private advance and rewind",
          "[rollback][stage-replay-presentation][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const auto directory = test::sampleLevel("detached-native-stage");
    writeTextFile(directory / "world.json", R"({"objects":[
      {"name":"FLOOR","position":[0,0,0],"flags":4}]})");
    writeTextFile(directory / "collision.json", R"({"objects":[
      {"object":0,"normals":[0,1,0,0,1,0],"vertices":[
      -200,0,-200,200,0,-200,200,0,200,-200,0,-200,200,0,200,-200,0,200]}]})");
    test::FakeRenderDevice device;
    MatchContext context;
    context.epoch = 3;
    context.scene = 1;
    context.owners = {1, 2, 0, 0};
    context.grants = {2, 5, 0, 0};
    PartyBootstrap::Party profiles;
    for (const usize seat : {usize{0}, usize{1}}) {
        CharacterSave save;
        save.name = "TEST";
        save.color = static_cast<s32>(seat);
        save.autoAttack = false;
        profiles[seat] = CharacterProfile::capture(save);
        REQUIRE(profiles[seat]);
    }
    auto prediction = std::make_unique<CombatPrediction>();
    REQUIRE(prediction->open(
        device, root, {.name = "TEST", .directory = directory.string(), .items = "ITEMS/levelG"},
        context, profiles, 1));
    auto seed = std::make_unique<OrdinaryCombatCheckpoint::State>();
    {
        auto captured = prediction->checkpoint(true);
        REQUIRE(captured);
        *seed = std::move(*captured);
    }
    seed->combat.players[0].actor.position = {-12, 0, 0};
    seed->combat.players[1].actor.position = {12, 0, 0};
    REQUIRE(prediction->begin(context.epoch, 100, *seed));
    OrdinaryCombatTick::Frame frame;
    frame.camera.position = {0, 35, -50};
    frame.camera.pitch = .6f;
    frame.projection.horizontalFov = 1.6f;
    frame.projection.aspect = 2;
    MotionSnapshot motion;
    motion.epoch = context.epoch;
    motion.tick = 100;
    motion.cameraContinuity = 1;
    motion.camera = frame.camera;
    motion.horizontalFov = frame.projection.horizontalFov;
    motion.aspect = frame.projection.aspect;
    for (const auto& player : prediction->players()) {
        const auto seat = static_cast<usize>(player.actor.player());
        motion.players[seat] =
            SeatMotion{context.grants[seat], 1, player.actor.position(), player.actor.yaw()};
    }
    auto baseline = std::make_unique<CombatSnapshot>();
    {
        auto captured = CombatCapture::capture(
            motion, prediction->players(), *prediction->enemies(), nullptr, prediction->world());
        REQUIRE(captured);
        *baseline = std::move(*captured);
    }
    baseline->geometry = prediction->world()->scene().geometry();
    baseline->opponents = seed->opponents;
    auto& opponents = baseline->opponentFrame.emplace();
    opponents.initialPlayers = 2;
    for (const auto& view : LevelOpponents::enemyViews(prediction->players())) {
        opponents.players.push_back({static_cast<u8>(view.player), view});
    }
    baseline->combatState = CombatStatePacket::State{seed->combat, seed->adapter};
    const auto catalog = StageStatePacket::Catalog::build(*seed->stage);
    REQUIRE(catalog);
    baseline->combatState->stage = StageStatePacket::encode(*seed->stage, *catalog);
    REQUIRE(baseline->combatState->stage);
    baseline->combatFrame = frame;
    REQUIRE(baseline->valid());
    OnlineCombatReplay replay;
    REQUIRE(replay.begin(context, 2, *prediction));
    InputCommand command;
    command.epoch = context.epoch;
    command.tick = 102;
    command.grant = context.grants[1];
    command.seat = 1;
    command.direction = {1, 0};
    command.magnitude = .5f;
    REQUIRE(replay.update(baseline.get(), std::span{&command, 1}));
    const auto publication = replay.publication();
    REQUIRE(publication);
    const auto key = Presenter::authority(context, 2, *baseline, prediction->resourceLifetime());
    REQUIRE(key);
    Presenter presenter;
    const Presenter::Time start{std::chrono::seconds{1}};
    REQUIRE(presenter.adopt(publication, *key, start));
    const auto sample = [&] {
        auto shown = std::make_unique<CombatSnapshot>(*baseline);
        auto presentation = provenance(*shown);
        REQUIRE(
            presenter.present(*shown, presentation, *key, start + std::chrono::milliseconds{20}));
        REQUIRE(presentation.particles);
        return bytes(*shown);
    };
    const auto expected = sample();
    const auto uploads = device.texturesCreated;
    const auto updates = device.textureUpdates;
    frame.damage.playSeconds += frame.seconds;
    frame.damage.playSeconds += frame.seconds;
    REQUIRE(prediction->advance(103, SessionInputs::Frame{}, frame).status ==
            OrdinaryCombatTick::Status::Advanced);
    CHECK_FALSE(replay.active());
    CHECK((sample() == expected));
    prediction->resetReplay();
    CHECK((sample() == expected));
    CHECK(device.texturesCreated == uploads);
    CHECK(device.textureUpdates == updates);
    // Stop/join would precede this on a worker path. No worker exists in this
    // test; clearing the sole owner expires the borrowed artwork fence.
    replay.clear();
    prediction->clear();
    CHECK_FALSE(presenter.active());
    auto shown = std::make_unique<CombatSnapshot>(*baseline);
    auto presentation = provenance(*shown);
    CHECK_FALSE(presenter.present(*shown, presentation, *key));
}
} // namespace
