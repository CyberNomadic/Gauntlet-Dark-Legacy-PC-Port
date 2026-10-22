#include <array>
#include <limits>
#include <memory>
#include <numbers>
#include <random>
#include <tuple>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/audio/SceneSoundEvents.h"
#include "game/critters/CombatantProjectilesCheckpoint.h"
#include "game/critters/Gargoyle.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Checkpoint = CombatantProjectilesCheckpoint;
static_assert(!std::is_copy_constructible_v<CombatantProjectiles>);
static_assert(!std::is_move_constructible_v<CombatantProjectiles>);

struct Fixture {
    test::FakeRenderDevice device;
    CombatantAssets stock;
    TextureAnimator textures;
    EffectTrees prototypes, effects, replayEffects;
    CombatantProjectiles native, replay;
    std::vector<EffectTrees::PoolResource> resources;
    std::unique_ptr<Checkpoint> owner, restored;
    std::vector<std::string> sounds;

    explicit Fixture(std::string_view name = "combatant-projectile-checkpoint") {
        const auto root = test::scratchDirectory(name);
        writeTextFile(root / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        writeFile(root / "white.png", test::kTinyPng);
        writeTextFile(root / "objects.json", R"({"objects":[{"name":"TRI","file":"tri.obj"}]})");
        writeTextFile(root / "textures.json",
                      R"({"bitmaps":[{"name":"WHITE","file":"white.png","width":2,"height":2}]})");
        writeTextFile(root / "animations.json", R"({"trees":[
          {"name":"SHOT","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],"sequences":[{"name":"BIRTH","frames":3,"frameRate":30}]},
          {"name":"LOOP","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}]},
          {"name":"HIT","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}]}]})");
        writeTextFile(root / "critter.json",
                      R"({"name":"TEST","types":[{"moveCount":1}],"descriptors":[{}],"moves":[{}],
          "damages":[
            {"type":1,"flags":32,"behaviorFlags":9,"radius":0.5,"damage":12,"minSpeed":30,"maxSpeed":30,"sfxIndex":0,"sfx":2,"morph":1,"morphEnd":2,"morphLife":0.5},
            {"type":1,"flags":2097184,"behaviorFlags":9,"radius":0.5,"damage":12,"minSpeed":30,"maxSpeed":30,"sfxIndex":3,"sfx":2},
            {"type":1,"flags":67108864,"behaviorFlags":9,"radius":0.5,"maxDistance":2,"damage":1,"minSpeed":30,"maxSpeed":30,"sfxIndex":3,"sfx":2},
            {"type":8,"flags":67108864,"radius":3,"maxDistance":3,"damage":1,"sfxIndex":0,"morph":1,"morphEnd":2,"morphLife":0.5},
            {"type":1,"flags":0,"behaviorFlags":9,"radius":0.5,"damage":0,"minSpeed":30,"maxSpeed":30,"sfxIndex":4,"sfx":2},
            {"type":1,"flags":0,"behaviorFlags":9,"radius":0.5,"damage":0,"minSpeed":30,"maxSpeed":30,"sfxIndex":5,"sfx":2},
            {"type":1,"flags":3145760,"behaviorFlags":9,"radius":0.5,"damage":12,"minSpeed":30,"maxSpeed":30,"sfxIndex":3,"sfx":2}],
          "sounds":[{"name":"SHOT","flags":8,"levelFormat":"S_%cSHOT"},{"name":"LOOP"},
            {"name":"HIT","flags":16,"levelFormat":"S_%cHIT"},{"name":"LOOP","life":100},
            {"name":"SHOT","flags":131072},{"name":"SHOT","flags":4194304}]})");
        test::convertModelFixture(root);
        REQUIRE(stock.archive.load(root));
        REQUIRE(stock.data.load(root / "critter.json"));
        bind();
    }
    void bind() {
        textures.bind(stock.archive.trees.textureAnimations(), stock.archive.textures, device);
        EffectTrees::Setting setting;
        setting.persistent = true;
        for (const auto* tree : {"SHOT", "LOOP", "HIT"}) {
            REQUIRE(prototypes.startSet(device, stock.archive, tree, {}, setting) != 0);
        }
        for (usize i = 0; i < prototypes.count(); ++i) {
            resources.push_back(
                {static_cast<u32>(i + 1), &stock.archive, {}, &textures, &prototypes.effect(i)});
        }
        const std::array catalog{Checkpoint::Resource{7, &stock}};
        owner = std::make_unique<Checkpoint>(native, effects, catalog, resources);
        restored = std::make_unique<Checkpoint>(replay, replayEffects, catalog, resources);
    }
    CombatShot shot(s32 row = 0) const {
        CombatShot value;
        value.data = &stock.data;
        value.critter = 11;
        value.damageIndex = row;
        value.origin = {0, 3, 0};
        value.target = Vec3{0, 3, 30};
        value.realm = 'G';
        return value;
    }
    void launch(s32 row = 0) {
        native.launch(
            shot(row), stock.archive, device, effects,
            [&](const CombatantProjectiles::SoundCue& cue) { sounds.emplace_back(cue.name); });
        REQUIRE(native.count() == 1);
    }
    void step(f32 seconds, std::span<const EnemyView> players = {},
              const WorldCollision* collision = nullptr, std::span<const MissileStop> stops = {}) {
        effects.capturePresentation();
        effects.update(seconds);
        native.update(
            seconds, collision, players, device, effects,
            [&](const CombatantProjectiles::SoundCue& cue) { sounds.emplace_back(cue.name); },
            stops);
    }
    void restorePair(const Checkpoint::State& state, const EffectTrees::PoolCheckpoint& shared) {
        auto pool = restored->prepareRestore(state, shared);
        auto fx = replayEffects.prepareRestorePool(shared, resources);
        REQUIRE(pool);
        REQUIRE(fx);
        REQUIRE(restored->canCommit(*pool));
        REQUIRE(replayEffects.canCommit(*fx));
        REQUIRE(replayEffects.commitRestore(std::move(*fx)));
        REQUIRE(restored->commitRestore(std::move(*pool)));
    }
    void equal() const {
        REQUIRE(owner->checkpoint());
        CHECK(owner->checkpoint() == restored->checkpoint());
        CHECK(effects.checkpointPool(resources) == replayEffects.checkpointPool(resources));
    }
};

TEST_CASE("combatant projectile checkpoint validates complete values without native resources",
          "[combatant-projectiles-checkpoint]") {
    CombatantProjectiles pool;
    CombatantProjectiles other;
    EffectTrees effects;
    Checkpoint owner(pool, effects, {}, {});
    Checkpoint foreign(other, effects, {}, {});
    const auto baseline = owner.checkpoint();
    REQUIRE(baseline);
    const auto shared = effects.checkpointPool();
    REQUIRE(shared);
    auto first = owner.prepareRestore(*baseline, *shared);
    auto sibling = owner.prepareRestore(*baseline, *shared);
    REQUIRE(first);
    REQUIRE(sibling);
    CHECK_FALSE(foreign.canCommit(*first));
    CHECK_FALSE(foreign.commitRestore(std::move(*first)));
    REQUIRE(owner.canCommit(*first));
    REQUIRE(owner.commitRestore(std::move(*first)));
    CHECK_FALSE(owner.canCommit(*first));
    CHECK_FALSE(owner.canCommit(*sibling));
    auto stale = owner.prepareRestore(*baseline, *shared);
    REQUIRE(stale);
    pool.clear(effects);
    CHECK_FALSE(owner.canCommit(*stale));
    auto bad = *baseline;
    bad.ricochetIn = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(bad.valid());
    bad = *baseline;
    bad.hits.resize(Checkpoint::kMaxOutputs + 1);
    CHECK_FALSE(bad.valid());
    bad = *baseline;
    bad.worldHits.push_back({{}, -2, false});
    CHECK_FALSE(bad.valid());
    bad = *baseline;
    bad.emittedEffects.push_back(0);
    CHECK_FALSE(bad.valid());
    bad = *baseline;
    bad.ricochetIn = 1.01f;
    CHECK_FALSE(bad.valid());
    bad = *baseline;
    bad.emittedEffects = {1, 1};
    CHECK_FALSE(bad.valid());
    bad.emittedEffects = {2, 1};
    CHECK_FALSE(bad.valid());
    bad = *baseline;
    bad.flying.resize(Checkpoint::kMaxFlights + 1);
    CHECK_FALSE(bad.valid());
}

TEST_CASE("native combatant projectile history restores RNG contact phases and every output queue",
          "[local-rewind][combatant-missile-local-rewind][native-equality]") {
    Fixture fixture;
    const s32 row = GENERATE(0, 1, 2, 3, 4, 5, 6);
    const bool reflecting = GENERATE(false, true);
    std::array<EnemyView, 2> players{EnemyView{0, {0, 0, 3}, 1, 6}, EnemyView{1, {0, 0, 6}, 1, 6}};
    players[0].reflects = reflecting;
    const std::array opponents{MissileTarget{32, {0, 0, -2}, 1, 8}};
    std::array<CombatantProjectiles::LocalSnapshot, 16> history;
    std::array<EffectTrees::LocalSnapshot, 16> visuals;
    std::array<CombatantProjectiles::LocalSnapshot, 8> expected;
    CombatantProjectiles::LocalSnapshot actual;
    const auto step = [&](CombatantProjectiles& pool, EffectTrees& effects, u32 frame, bool wrong) {
        effects.capturePresentation();
        if (frame % 5 == 0) {
            auto shot = fixture.shot(row);
            shot.target = wrong ? Vec3{5, 9, -10} : Vec3{0, 3, 20};
            pool.launch(shot, fixture.stock.archive, fixture.device, effects, {});
        }
        effects.update(1.0f / 60);
        pool.update(1.0f / 60, nullptr, players, fixture.device, effects, {}, {}, opponents);
    };
    for (u32 frame = 0; frame < 64; ++frame) {
        CAPTURE(frame, row, reflecting);
        const u32 slot = frame % history.size();
        fixture.replay.saveLocal(history[slot]);
        fixture.replayEffects.saveLocal(visuals[slot]);
        const u32 distance = frame % 8 + 1;
        for (u32 i = 0; i < distance; ++i) {
            step(fixture.replay, fixture.replayEffects, frame + i, false);
            fixture.replay.saveLocal(expected[i]);
        }
        REQUIRE(fixture.replay.restoreLocal(history[slot]));
        REQUIRE(fixture.replayEffects.restoreLocal(visuals[slot]));
        for (u32 i = 0; i < distance; ++i) {
            step(fixture.replay, fixture.replayEffects, frame + i, true);
        }
        (void)fixture.replay.takeHits();
        (void)fixture.replay.takeRockHits();
        (void)fixture.replay.takeWorldHits();
        (void)fixture.replay.takeGenerators();
        (void)fixture.replay.takeSummons();
        REQUIRE(fixture.replay.canRestoreLocal(history[slot]));
        REQUIRE(fixture.replayEffects.canRestoreLocal(visuals[slot]));
        REQUIRE(fixture.replay.restoreLocal(history[slot]));
        REQUIRE(fixture.replayEffects.restoreLocal(visuals[slot]));
        // The retained diagnostic captures observations only. Native restoration
        // above is an ordinary value copy, not prepare/rebind/commit.
        fixture.equal();
        for (u32 i = 0; i < distance; ++i) {
            step(fixture.native, fixture.effects, frame + i, false);
            step(fixture.replay, fixture.replayEffects, frame + i, false);
            fixture.replay.saveLocal(actual);
            CHECK(fixture.replay.equalLocal(expected[i], actual));
            fixture.equal();
        }
    }
    CHECK(fixture.device.textureUpdates == 0);
}

TEST_CASE("native combatant projectile rewind rejects foreign owners and stale prepared restores",
          "[local-rewind][combatant-missile-local-rewind][native-equality]") {
    CombatantProjectiles a;
    CombatantProjectiles b;
    EffectTrees effects;
    CombatantProjectiles::LocalSnapshot saved;
    CHECK_FALSE(a.equalLocal(saved, saved));
    CHECK_FALSE(a.restoreLocal(saved));
    a.saveLocal(saved);
    CHECK(a.equalLocal(saved, saved));
    CHECK_FALSE(b.equalLocal(saved, saved));
    CHECK_FALSE(b.restoreLocal(saved));
    const Checkpoint observer(a, effects, {}, {});
    const auto state = observer.checkpoint();
    const auto visual = effects.checkpointPool();
    REQUIRE(state);
    REQUIRE(visual);
    auto prepared = observer.prepareRestore(*state, *visual);
    REQUIRE(prepared);
    REQUIRE(a.restoreLocal(saved));
    CHECK_FALSE(observer.canCommit(*prepared));
    a.clear(effects);
    CHECK_FALSE(a.equalLocal(saved, saved));
    CHECK_FALSE(a.restoreLocal(saved));
    {
        const CombatantProjectiles temporary;
        temporary.saveLocal(saved);
    }
    CHECK_FALSE(a.equalLocal(saved, saved));
    CHECK_FALSE(a.restoreLocal(saved));
}

TEST_CASE("native combatant projectile equality retains undelivered hit queues",
          "[local-rewind][combatant-missile-local-rewind][native-equality]") {
    Fixture fixture;
    fixture.launch();
    const std::array players{EnemyView{0, {0, 0, 3}, 1, 6}};
    fixture.step(0.3f, players);
    CombatantProjectiles::LocalSnapshot before;
    CombatantProjectiles::LocalSnapshot after;
    fixture.native.saveLocal(before);
    const auto hits = fixture.native.takeHits();
    REQUIRE_FALSE(hits.empty());
    fixture.native.saveLocal(after);
    CHECK_FALSE(fixture.native.equalLocal(before, after));
    REQUIRE(fixture.native.restoreLocal(before));
    fixture.native.saveLocal(after);
    CHECK(fixture.native.equalLocal(before, after));
    CHECK(fixture.native.takeHits() == hits);
}

TEST_CASE("combatant launch uses portable native float mapping and exact random draw cadence",
          "[combatant-projectiles-checkpoint]") {
    Fixture fixture;
    Mt19937 expected;
    std::mt19937 standard;
    for (s32 index = 0; index < 12; ++index) {
        const auto shot = fixture.shot(index % 2 == 0 ? 0 : 1);
        const auto* damage = shot.data->damage(shot.damageIndex);
        REQUIRE(damage);
        const auto* cue = shot.data->sound(damage->sound);
        REQUIRE(cue);
        const bool spinning = (cue->flags & 8U) != 0;
        const f32 spread = expected.unitFloat24() * 2.0f - 1.0f;
        Vec3 spin{0};
        constexpr f32 kRate = std::numbers::pi_v<f32> / 2.0f;
        if (spinning) {
            spin = {expected.unitFloat24() * kRate, 0, expected.unitFloat24() * kRate};
        }
#if defined(_MSC_VER)
        // This is the pre-migration native implementation on the reference
        // compiler. Other standard libraries intentionally use the explicit mapping.
        CHECK(spread == std::uniform_real_distribution<f32>{-1, 1}(standard));
        if (spinning) {
            CHECK(spin.x == std::uniform_real_distribution<f32>{0, kRate}(standard));
            CHECK(spin.z == std::uniform_real_distribution<f32>{0, kRate}(standard));
        }
#else
        standard.discard(spinning ? 3 : 1);
#endif
        fixture.native.launch(shot, fixture.stock.archive, fixture.device, fixture.effects, {});
        const auto captured = fixture.owner->checkpoint();
        REQUIRE(captured);
        REQUIRE(captured->flying.size() == static_cast<usize>(index + 1));
        CHECK(captured->random == expected.checkpoint());
        CHECK(captured->flying.back().velocity ==
              CombatantProjectile::velocity(*damage, shot, spread));
        CHECK(captured->flying.back().spin == spin);
        auto sample = expected;
        auto standardSample = standard;
        CHECK(sample() == standardSample());
    }
    auto malformed = *fixture.owner->checkpoint();
    malformed.random.cursor = static_cast<u32>(Mt19937::state_size + 1);
    CHECK_FALSE(malformed.valid());
    malformed = *fixture.owner->checkpoint();
    malformed.random.words.fill(0);
    CHECK_FALSE(malformed.valid());
}

TEST_CASE("combatant sound phases and linked SFXX rows retain their own identities",
          "[native-combat-audio][combatant-missile-local-rewind]") {
    Fixture fixture;
    const auto root = test::scratchDirectory("combatant-sound-phases");
    writeTextFile(root / "critter.json", R"({"name":"PHASES","types":[{"moveCount":1}],
      "descriptors":[{}],"moves":[{}],"damages":[{"type":1,"behaviorFlags":9,
      "minSpeed":30,"maxSpeed":30,"sfxIndex":0,"sfx":0,"morph":1,"morphEnd":0,
      "morphLife":0.1}],"sounds":[
      {"name":"SHOT","levelFormat":"S_SHARED","link":2},
      {"name":"LOOP","levelFormat":"S_MORPH"},
      {"name":"WHITE","flags":33570816,"life":1,"rate":5,"custom1":500,
       "levelFormat":"S_LINK"}]})");
    CritterData data;
    REQUIRE(data.load(root / "critter.json"));
    auto shot = fixture.shot();
    shot.data = &data;
    const SoundClip clip{24000, 1, std::vector<f32>(24000, 0.125f)};
    const SoundSequence sequence{{{&clip, false, false}}, 1, SoundGainCurve::Linear, "TEST", 1};
    SceneSoundEvents sounds;
    sounds.open(1, nullptr);
    sounds.beginTick(1);
    std::vector<std::tuple<u64, CombatantProjectiles::SoundPhase, s32, std::string>> cues;
    const auto play = [&](const CombatantProjectiles::SoundCue& cue) {
        cues.emplace_back(cue.instance, cue.phase, cue.effect, cue.name);
        auto scope = sounds.scoped({23, static_cast<u64>(cue.effect) + 1, cue.instance},
                                   static_cast<u32>(cue.phase));
        sounds.emit(sequence);
    };
    fixture.native.launch(shot, fixture.stock.archive, fixture.device, fixture.effects, play);
    fixture.effects.update(0.11f);
    fixture.native.update(0.11f, nullptr, {}, fixture.device, fixture.effects, play);
    fixture.effects.update(0.11f);
    fixture.native.update(0.11f, nullptr, {}, fixture.device, fixture.effects, play);
    const auto frame = sounds.endFrame();
    using Phase = CombatantProjectiles::SoundPhase;
    REQUIRE(cues.size() == 5);
    const u64 birth = std::get<0>(cues[0]);
    REQUIRE(birth != 0);
    CHECK((cues[0] == std::tuple{birth, Phase::Launch, 0, std::string{"S_SHARED"}}));
    CHECK((cues[1] == std::tuple{birth, Phase::Launch, 2, std::string{"S_LINK"}}));
    CHECK((cues[2] == std::tuple{birth, Phase::Morph, 1, std::string{"S_MORPH"}}));
    CHECK((cues[3] == std::tuple{birth, Phase::End, 0, std::string{"S_SHARED"}}));
    CHECK((cues[4] == std::tuple{birth, Phase::End, 2, std::string{"S_LINK"}}));
    REQUIRE(frame.finite.events.size() == cues.size());
    for (const auto& event : frame.finite.events) {
        CHECK(event.id.occurrence == 0); // no unrelated phase or linked row advances this cue
    }
    REQUIRE(sounds.reconcileFrames(1, std::span{&frame, 1}));
}

TEST_CASE("corrected combatant impacts do not steal another flight's already presented sound",
          "[native-combat-audio][combatant-missile-local-rewind][scene-sound-events]") {
    Fixture fixture;
    for (const f32 x : {0.0f, 20.0f}) {
        auto shot = fixture.shot();
        shot.origin.x = x;
        shot.target->x = x;
        fixture.native.launch(shot, fixture.stock.archive, fixture.device, fixture.effects, {});
    }
    const auto initial = fixture.owner->checkpoint();
    REQUIRE(initial);
    REQUIRE(initial->flying.size() == 2);
    const u64 first = initial->flying[0].instance;
    const u64 second = initial->flying[1].instance;
    REQUIRE(first != second);
    CombatantProjectiles::LocalSnapshot saved;
    EffectTrees::LocalSnapshot savedEffects;
    fixture.native.saveLocal(saved);
    fixture.effects.saveLocal(savedEffects);
    const SoundClip clip{24000, 1, std::vector<f32>(24000, 0.125f)};
    const SoundSequence sequence{{{&clip, false, false}}, 1, SoundGainCurve::Linear, "TEST", 1};
    AudioMixer mixer(24000);
    SoundPlayer player(mixer);
    SceneSoundEvents sounds;
    sounds.open(7, &player);
    usize commands = 0;
    const SoundPlayer::Capture observe(&player, [&](const auto&) { ++commands; });
    const auto step = [&](u64 tick, bool both) {
        std::array views{EnemyView{0, {0, 0, 0.5f}, 1, 6}, EnemyView{1, {20, 0, 0.5f}, 1, 6}};
        views[0].hidden = !both;
        sounds.beginTick(tick);
        fixture.effects.update(1.0f / 60);
        fixture.native.update(1.0f / 60, nullptr, views, fixture.device, fixture.effects,
                              [&](const CombatantProjectiles::SoundCue& cue) {
                                  auto scope = sounds.scoped(
                                      {23, static_cast<u64>(cue.effect) + 1, cue.instance},
                                      static_cast<u32>(cue.phase));
                                  sounds.emit(sequence);
                              });
        return sounds.endFrame();
    };
    const auto wrong = step(1, true);
    REQUIRE(wrong.finite.events.size() == 2);
    CHECK(wrong.finite.events[0].id.emitter == FrameEventEmitter{23, 3, first});
    CHECK(wrong.finite.events[1].id.emitter == FrameEventEmitter{23, 3, second});
    CHECK(commands == 0);
    const auto published = sounds.reconcileFrames(1, std::span{&wrong, 1});
    REQUIRE(published);
    sounds.dispatch(published);
    REQUIRE(commands > 0);
    const usize presented = commands;
    for (u64 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        REQUIRE(fixture.native.restoreLocal(saved));
        REQUIRE(fixture.effects.restoreLocal(savedEffects));
        for (u64 tick = 1; tick <= distance; ++tick) {
            step(tick, true);
        }
        REQUIRE(fixture.native.restoreLocal(saved));
        REQUIRE(fixture.effects.restoreLocal(savedEffects));
        std::vector<SceneSoundEvents::Frame> corrected;
        for (u64 tick = 1; tick <= distance; ++tick) {
            corrected.push_back(step(tick, false));
        }
        REQUIRE(corrected[0].finite.events.size() == 1);
        CHECK(corrected[0].finite.events[0] == wrong.finite.events[1]);
        for (usize frame = 1; frame < corrected.size(); ++frame) {
            CHECK(corrected[frame].finite.events.empty());
        }
        const auto delta = sounds.reconcileFrames(1, corrected);
        REQUIRE(delta);
        CHECK(delta.actions.empty());
        sounds.dispatch(delta);
        CHECK(commands == presented);
    }
    CHECK(sounds.unsupportedCount() == 0);
}

TEST_CASE("combatant hit queues retain retired births when compacted slots are reused",
          "[native-combat-audio][combatant-missile-local-rewind][native-equality]") {
    Fixture fixture;
    fixture.launch();
    const auto initial = fixture.owner->checkpoint();
    REQUIRE(initial);
    const u64 retired = initial->flying.front().instance;
    const std::array views{EnemyView{0, {0, 0, 0.5f}, 1, 6}};
    fixture.step(1.0f / 60, views);
    REQUIRE(fixture.native.count() == 0);
    fixture.launch();
    const auto replacement = fixture.owner->checkpoint();
    REQUIRE(replacement);
    REQUIRE(replacement->flying.size() == 1);
    REQUIRE(replacement->hits.size() == 1);
    CHECK(replacement->hits[0].instance == retired);
    CHECK(replacement->flying[0].instance != retired);
    CombatantProjectiles::LocalSnapshot saved;
    CombatantProjectiles::LocalSnapshot drained;
    fixture.native.saveLocal(saved);
    const auto queued = fixture.native.takeHits();
    fixture.native.saveLocal(drained);
    CHECK_FALSE(fixture.native.equalLocal(saved, drained));
    REQUIRE(fixture.native.restoreLocal(saved));
    CHECK(fixture.native.takeHits() == queued);
    auto invalid = *replacement;
    invalid.flying[0].instance = 0;
    CHECK_FALSE(invalid.valid());
    invalid = *replacement;
    invalid.hits[0].instance = invalid.nextInstance;
    CHECK_FALSE(invalid.valid());
    invalid = *replacement;
    invalid.worldHits.push_back({{}, -1, true, invalid.nextInstance});
    CHECK_FALSE(invalid.valid());
    fixture.native.clear(fixture.effects);
    fixture.launch();
    REQUIRE(fixture.owner->checkpoint());
    CHECK(fixture.owner->checkpoint()->flying[0].instance > replacement->flying[0].instance);
}

TEST_CASE("combatant projectile checkpoints restore independently loaded native resources",
          "[combatant-projectiles-checkpoint]") {
    Fixture host("combatant-projectile-cross-host");
    Fixture guest("combatant-projectile-cross-guest");
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    host.launch();
    host.step(0.075f);
    const auto saved = host.owner->checkpoint();
    const auto shared = host.effects.checkpointPool(host.resources);
    REQUIRE(saved);
    REQUIRE(shared);
    REQUIRE(&host.stock.archive != &guest.stock.archive);
    const auto uploads = guest.device.texturesCreated;
    const auto updates = guest.device.textureUpdates;
    guest.restorePair(*saved, *shared);
    CHECK(guest.restored->checkpoint() == saved);
    CHECK(guest.replayEffects.checkpointPool(guest.resources) == shared);
    for (s32 tick = 0; tick < 45; ++tick) {
        CAPTURE(tick, seconds);
        if (tick == 4) {
            host.native.launch(host.shot(1), host.stock.archive, host.device, host.effects, {});
            guest.replay.launch(guest.shot(1), guest.stock.archive, guest.device,
                                guest.replayEffects, {});
        }
        host.step(seconds);
        guest.replayEffects.capturePresentation();
        guest.replayEffects.update(seconds);
        guest.replay.update(seconds, nullptr, {}, guest.device, guest.replayEffects, {});
        REQUIRE(host.owner->checkpoint());
        CHECK(guest.restored->checkpoint() == host.owner->checkpoint());
        CHECK(guest.replayEffects.checkpointPool(guest.resources) ==
              host.effects.checkpointPool(host.resources));
    }
    CHECK(guest.sounds.empty());
    CHECK(guest.device.texturesCreated == uploads);
    CHECK(guest.device.textureUpdates == updates);
    CHECK(guest.device.draws.empty());
}

TEST_CASE("expired shared effects remain pending native projectile cleanup across rewind",
          "[combatant-projectiles-checkpoint]") {
    Fixture f;
    f.launch(1);
    const auto live = f.owner->checkpoint();
    REQUIRE(live);
    REQUIRE(live->flying.size() == 1);
    f.effects.stop(live->flying.front().effect);
    const auto pending = f.owner->checkpoint();
    const auto shared = f.effects.checkpointPool(f.resources);
    REQUIRE(pending);
    REQUIRE(shared);
    REQUIRE(shared->effects.empty());
    REQUIRE(pending->flying.size() == 1);
    REQUIRE_FALSE(pending->emittedEffects.empty());
    const auto sounds = f.sounds;
    f.restorePair(*pending, *shared);
    f.equal();
    CHECK(f.sounds == sounds);
    f.step(1.0f / 60);
    f.replayEffects.capturePresentation();
    f.replayEffects.update(1.0f / 60);
    f.replay.update(1.0f / 60, nullptr, {}, f.device, f.replayEffects, {});
    f.equal();
    REQUIRE(f.owner->checkpoint());
    CHECK(f.owner->checkpoint()->flying.empty());
    CHECK(f.owner->checkpoint()->emittedEffects.empty());
}

TEST_CASE("native combatant flights rewind phases contacts reflection and future random draws",
          "[combatant-projectiles-checkpoint]") {
    const f32 dt = GENERATE(1.0f / 30, 1.0f / 60);
    const s32 row = GENERATE(0, 1, 2, 3, 6);
    CAPTURE(dt, row);
    Fixture f;
    f.launch(row);
    std::array players{EnemyView{.player = 2, .position = {0, 0, 4}, .radius = 1, .height = 6}};
    players[0].reflects = row == 1;
    f.step(row == 1 || row == 2 || row == 6 ? 0.1f : 0.075f, players);
    const auto saved = f.owner->checkpoint();
    const auto shared = f.effects.checkpointPool(f.resources);
    REQUIRE(saved);
    REQUIRE(shared);
    REQUIRE_FALSE(saved->flying.empty());
    if (row == 1) {
        REQUIRE(saved->flying.front().reflected);
        REQUIRE_FALSE(saved->hits.empty());
        REQUIRE(saved->ricochetIn > 0);
    }
    const auto uploads = f.device.texturesCreated;
    const auto sounds = f.sounds;
    f.restorePair(*saved, *shared);
    f.equal();
    CHECK(f.sounds == sounds);
    CHECK(f.device.texturesCreated == uploads);
    for (s32 tick = 0; tick < 80; ++tick) {
        CAPTURE(tick);
        if (tick == 12) {
            // A new launch checks the future RNG stream, not just saved velocity.
            f.native.launch(f.shot(), f.stock.archive, f.device, f.effects, {});
            f.replay.launch(f.shot(), f.stock.archive, f.device, f.replayEffects, {});
        }
        f.step(dt, players);
        f.replayEffects.capturePresentation();
        f.replayEffects.update(dt);
        f.replay.update(dt, nullptr, players, f.device, f.replayEffects, {});
        f.equal();
    }
    CHECK(f.device.texturesCreated == uploads);
    CHECK(f.device.draws.empty());
}

TEST_CASE("combatant projectile checkpoints retain rock world generator and summon queues",
          "[combatant-projectiles-checkpoint]") {
    const s32 event = GENERATE(0, 1, 2, 3);
    Fixture f;
    f.launch(event == 2 ? 4 : event == 3 ? 5 : 1);
    if (event == 0) {
        Obstacle cover;
        cover.centre = {0, 0, 2};
        cover.height = 8;
        cover.halfAcross = 5;
        cover.halfAlong = 0.5f;
        const std::array stops{MissileStop{.box = cover, .rock = 3, .rockHealth = 90}};
        f.step(0.1f, {}, nullptr, stops);
    } else if (event == 1) {
        WorldCollision world;
        CollisionTriangle wall;
        wall.normal = {0, 0, -1};
        wall.vertices = {Vec3{-10, 0, 2}, Vec3{0, 20, 2}, Vec3{10, 0, 2}};
        world.build({wall});
        f.step(0.1f, {}, &world);
    } else {
        f.step(0.25f);
    }
    const auto saved = f.owner->checkpoint();
    const auto shared = f.effects.checkpointPool(f.resources);
    REQUIRE(saved);
    REQUIRE(shared);
    if (event == 0) {
        REQUIRE_FALSE(saved->rockHits.empty());
    }
    if (event == 1) {
        REQUIRE_FALSE(saved->worldHits.empty());
    }
    if (event == 2) {
        REQUIRE_FALSE(saved->generators.empty());
    }
    if (event == 3) {
        REQUIRE_FALSE(saved->summons.empty());
    }
    f.restorePair(*saved, *shared);
    f.equal();
    CHECK(f.native.takeRockHits() == f.replay.takeRockHits());
    CHECK(f.native.takeGenerators() == f.replay.takeGenerators());
    CHECK(f.native.takeSummons() == f.replay.takeSummons());
    f.equal();
}

TEST_CASE("late malformed flight resource and shared effect topology refuse atomically",
          "[combatant-projectiles-checkpoint]") {
    Fixture f;
    f.launch();
    const auto saved = f.owner->checkpoint();
    const auto shared = f.effects.checkpointPool(f.resources);
    REQUIRE(saved);
    REQUIRE(shared);
    for (s32 mutation = 0; mutation < 14; ++mutation) {
        CAPTURE(mutation);
        auto bad = *saved;
        auto badFx = *shared;
        switch (mutation) {
        case 0: bad.flying[0].shot.definition.resource = 99; break;
        case 1: bad.flying[0].shot.definition.index = 1; break;
        case 2: bad.flying[0].shot.damageIndex = 4000; break;
        case 3: bad.flying[0].archive = 99; break;
        case 4: bad.flying[0].effect = shared->nextId; break;
        case 5: bad.emittedEffects.push_back(shared->nextId); break;
        case 6: bad.flying[0].velocity.y = std::numeric_limits<f32>::infinity(); break;
        case 7: bad.flying[0].piercedPlayer = 4; break;
        case 8: badFx.effects[0].resource = 999; break;
        case 9: bad.catalog[0].name = "WRONG_BINDING"; break;
        case 10: bad.flying.push_back(bad.flying[0]); break;
        case 11: bad.flying[0].contactSeconds = 1.0f / 30; break;
        case 12:
            bad.flying[0].planted = true;
            bad.flying[0].stuck = true;
            break;
        case 13: bad.flying[0].morphed = true; break;
        default: break;
        }
        CHECK_FALSE(f.owner->prepareRestore(bad, badFx));
        CHECK(f.owner->checkpoint() == saved);
        CHECK(f.effects.checkpointPool(f.resources) == shared);
    }
    // Code-attached topology is not represented by a plain tree prototype.
    ParticleDescriptor descriptor;
    descriptor.rate.fill(1);
    descriptor.particleLife = 10;
    f.effects.attachTrail(saved->flying.front().effect, descriptor,
                          f.stock.archive.textures.texture(f.device, 0));
    CHECK_FALSE(f.effects.checkpointPool(f.resources));
    CHECK_FALSE(f.owner->checkpoint());
    CHECK(f.native.count() == 1);
}

TEST_CASE("missile resource lifetime is independent of shooter reuse but not stock reload",
          "[combatant-projectiles-checkpoint]") {
    Fixture f;
    f.launch(1);
    const auto saved = f.owner->checkpoint();
    const auto shared = f.effects.checkpointPool(f.resources);
    REQUIRE(saved);
    REQUIRE(shared);
    REQUIRE(saved->flying.front().shot.critter == 11);
    // No current actor is consulted. A shot remains bound to the immutable stock,
    // even when no actor with its old native slot exists at the destination.
    f.restorePair(*saved, *shared);
    f.equal();
    auto prepared = f.owner->prepareRestore(*saved, *shared);
    REQUIRE(prepared);
    f.native.clear(f.effects);
    CHECK_FALSE(f.owner->canCommit(*prepared));
    // Teardown borrowers before the stock. This checks invalidation, not use of
    // destroyed render resources; no native update or render follows the clear.
    f.replay.clear(f.replayEffects);
    auto stockPlan = f.restored->prepareRestore(*saved, *shared);
    REQUIRE(stockPlan);
    f.effects.clear();
    f.replayEffects.clear();
    f.prototypes.clear();
    f.stock.clear();
    CHECK_FALSE(f.restored->canCommit(*stockPlan));
    CHECK_FALSE(f.restored->checkpoint());
}

TEST_CASE("native gargoyle birth and linked topology restore independently of a live shooter",
          "[combatant-projectiles-checkpoint][assets]") {
    const auto* form = GENERATE("GAR_EAGL", "GAR_LION", "GAR_SERP");
    CAPTURE(form);
    const auto root = test::assetOrSkip("CRITTER/GAR_EAGL.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    CombatantAssets stock;
    REQUIRE(stock.load(device, root, Gargoyle::definition(form), 'G'));
    const auto move = stock.data.moveNamed("FBALL");
    REQUIRE(move);
    CombatShot shot;
    shot.data = &stock.data;
    shot.damageIndex = stock.data.moves()[*move].damage0;
    shot.critter = 9;
    shot.origin = {0, 12, 0};
    shot.target = Vec3{5, 6, 30};
    EffectTrees prototype;
    EffectTrees effects;
    EffectTrees restoredEffects;
    CombatantProjectiles warmup;
    CombatantProjectiles pool;
    CombatantProjectiles restoredPool;
    // Preload the exact native birth topology, including any authored SFXX
    // trails, before measuring restoration. This is not runtime speculative
    // emission or an assertion that the Stage catalog already supports it.
    warmup.launch(shot, stock.archive, device, prototype, {});
    REQUIRE(warmup.count() == 1);
    REQUIRE(prototype.count() == 1);
    TextureAnimator textures;
    textures.bind(stock.archive.trees.textureAnimations(), stock.archive.textures, device);
    const std::array resources{
        EffectTrees::PoolResource{51, &stock.archive, {}, &textures, &prototype.effect(0)}};
    const std::array stocks{Checkpoint::Resource{73, &stock}};
    const Checkpoint native(pool, effects, stocks, resources);
    Checkpoint replay(restoredPool, restoredEffects, stocks, resources);
    pool.launch(shot, stock.archive, device, effects, {});
    REQUIRE(pool.count() == 1);
    const auto baseline = native.checkpoint();
    const auto shared = effects.checkpointPool(resources);
    REQUIRE(baseline);
    REQUIRE(shared);
    REQUIRE((baseline->flying.front().shot.definition == Checkpoint::Definition{73, 0}));
    const auto uploads = device.texturesCreated;
    auto missiles = replay.prepareRestore(*baseline, *shared);
    auto visual = restoredEffects.prepareRestorePool(*shared, resources);
    REQUIRE(missiles);
    REQUIRE(visual);
    REQUIRE(restoredEffects.commitRestore(std::move(*visual)));
    REQUIRE(replay.commitRestore(std::move(*missiles)));
    CHECK(native.checkpoint() == replay.checkpoint());
    // No actor with id 9 exists in either fixture. All mutable launch state and
    // future RNG still resume using the immutable authored definition.
    for (s32 tick = 0; tick < 2; ++tick) {
        effects.update(1.0f / 60);
        restoredEffects.update(1.0f / 60);
        pool.update(1.0f / 60, nullptr, {}, device, effects, {});
        restoredPool.update(1.0f / 60, nullptr, {}, device, restoredEffects, {});
        REQUIRE(native.checkpoint());
        CHECK(native.checkpoint() == replay.checkpoint());
        CHECK(effects.checkpointPool(resources) == restoredEffects.checkpointPool(resources));
    }
    CHECK(device.texturesCreated == uploads);
    CHECK(device.draws.empty());
}
} // namespace
