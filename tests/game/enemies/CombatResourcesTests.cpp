#include <algorithm>
#include <array>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/enemies/CombatResources.h"
#include "game/party/AttackCheckpointFixture.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(!std::is_move_constructible_v<CombatResources>);

ParticleDescriptor particleDescriptor() {
    ParticleDescriptor descriptor;
    descriptor.texture = "SKIN";
    descriptor.rate = {2, 2, 2, 2};
    descriptor.rateRandom = 0;
    descriptor.emitFrames = 12;
    descriptor.particleLife = 30;
    descriptor.particleFade = 9;
    descriptor.maxParticles = 16;
    descriptor.width = {1, 1, 1, 0};
    descriptor.alpha = {255, 255, 128, 0};
    return descriptor;
}

TEST_CASE("combat resources retain native bank clocks without any surviving effect",
          "[combat-resources]") {
    test::AttackCheckpointFixture source;
    test::FakeRenderDevice otherDevice;
    ItemArchive other;
    REQUIRE(other.load(source.root / "WEAPONS"));
    CombatResources host;
    CombatResources guest;
    // Fixed authored IDs win over declaration order or alias count. The actual
    // archives/textures belong to different owners and cannot compare by address.
    const std::array hostBanks{CombatResources::Bank{19, &source.weapons, {}},
                               CombatResources::Bank{7, &source.weapons, {}}};
    const std::array guestBanks{CombatResources::Bank{7, &other, {}},
                                CombatResources::Bank{19, &other, {}}};
    REQUIRE(host.bind(source.device, hostBanks));
    REQUIRE(guest.bind(otherDevice, guestBanks));
    CHECK(host.bankCount() == 1);
    CHECK(host.effectCount() == guest.effectCount());
    EffectTrees native;
    EffectTrees replay;
    const u32 started = native.startSet(source.device, source.weapons, "BURST", {}, {});
    REQUIRE(started != 0);
    native.update(0.1375f);
    native.stop(started);
    REQUIRE(native.count() == 0);
    REQUIRE_FALSE(native.checkpointPool());
    const auto captured = native.checkpointPool(host.resources().sharedEffects);
    REQUIRE(captured);
    REQUIRE(captured->banks.size() == 1);
    CHECK(captured->banks[0].resource == (7U << 16));
    CHECK(captured->banks[0].clock != TextureAnimator::Checkpoint{});
    const auto hostUploads = source.device.texturesCreated;
    const auto guestUploads = otherDevice.texturesCreated;
    REQUIRE(replay.restorePool(*captured, guest.resources().sharedEffects));
    CHECK(replay.checkpointPool(guest.resources().sharedEffects) == captured);
    native.update(0.05f);
    replay.update(0.05f);
    CHECK(replay.checkpointPool(guest.resources().sharedEffects) ==
          native.checkpointPool(host.resources().sharedEffects));
    CHECK(source.device.texturesCreated == hostUploads);
    CHECK(otherDevice.texturesCreated == guestUploads);
}

TEST_CASE("combat resource banks preserve exact ordered and repeated native lenders",
          "[combat-resources]") {
    test::AttackCheckpointFixture source;
    ItemArchive other;
    TextureSet hostExtra;
    TextureSet guestExtra;
    test::FakeRenderDevice otherDevice;
    REQUIRE(other.load(source.root / "WEAPONS"));
    REQUIRE(hostExtra.load(source.root / "WEAPONS"));
    REQUIRE(guestExtra.load(source.root / "WEAPONS"));
    const std::vector<TextureSet*> hostFirst{&source.weapons.textures, &hostExtra,
                                             &source.weapons.textures};
    const std::vector<TextureSet*> hostSecond{&hostExtra, &source.weapons.textures};
    const std::vector<TextureSet*> guestFirst{&other.textures, &guestExtra, &other.textures};
    const std::vector<TextureSet*> guestSecond{&guestExtra, &other.textures};
    const std::array hostBanks{CombatResources::Bank{2, &source.weapons, hostFirst},
                               CombatResources::Bank{3, &source.weapons, hostSecond}};
    const std::array guestBanks{CombatResources::Bank{3, &other, guestSecond},
                                CombatResources::Bank{2, &other, guestFirst}};
    CombatResources host;
    CombatResources guest;
    REQUIRE(host.bind(source.device, hostBanks));
    REQUIRE(guest.bind(otherDevice, guestBanks));
    CHECK(host.bankCount() == 2);
    EffectTrees native;
    EffectTrees replay;
    REQUIRE(native.startSet(source.device, source.weapons, "BURST", {}, {}, hostSecond) != 0);
    native.update(0.05f);
    REQUIRE(native.startSet(source.device, source.weapons, "BLOCKFX", {}, {}, hostFirst) != 0);
    native.update(0.03f);
    const auto state = native.checkpointPool(host.resources().sharedEffects);
    REQUIRE(state);
    REQUIRE(state->banks.size() == 2);
    CHECK(state->banks[0].resource == (3U << 16));
    CHECK(state->banks[1].resource == (2U << 16));
    REQUIRE(replay.restorePool(*state, guest.resources().sharedEffects));
    for (s32 tick = 0; tick < 150; ++tick) {
        native.capturePresentation();
        replay.capturePresentation();
        native.update(1.0f / 60);
        replay.update(1.0f / 60);
        CHECK(replay.checkpointPool(guest.resources().sharedEffects) ==
              native.checkpointPool(host.resources().sharedEffects));
    }
}

TEST_CASE("combat resources restore live native particles and their retirement tails",
          "[combat-resources]") {
    const bool retiring = GENERATE(false, true);
    test::AttackCheckpointFixture source;
    ItemArchive other;
    test::FakeRenderDevice otherDevice;
    REQUIRE(other.load(source.root / "WEAPONS"));
    const auto slot = source.weapons.textures.find("SKIN");
    REQUIRE(slot);
    const auto descriptor = particleDescriptor();
    const std::array hostBanks{CombatResources::Bank{4, &source.weapons, {}}};
    const std::array guestBanks{CombatResources::Bank{4, &other, {}}};
    const std::array particles{CombatResources::Particle{37, 4, descriptor, *slot}};
    CombatResources host;
    CombatResources guest;
    REQUIRE(host.bind(source.device, hostBanks, {}, particles));
    REQUIRE(guest.bind(otherDevice, guestBanks, {}, particles));
    EffectTrees native;
    EffectTrees replay;
    const u32 id = native.startParticles(source.device, source.weapons, descriptor, *slot, Mat4{1});
    REQUIRE(id != 0);
    native.update(0.1f);
    if (retiring) {
        native.finish(id);
    }
    const auto state = native.checkpointPool(host.resources().sharedEffects);
    REQUIRE(state);
    REQUIRE(state->effects.size() == 1);
    CHECK(state->effects[0].resource == (0x80000000U | 37U));
    REQUIRE_FALSE(state->effects[0].state.trails.entries[0].emitter.particles.empty());
    const auto uploads = otherDevice.texturesCreated;
    REQUIRE(replay.restorePool(*state, guest.resources().sharedEffects));
    for (s32 tick = 0; tick < 120; ++tick) {
        native.update(1.0f / 60);
        replay.update(1.0f / 60);
        CHECK(replay.checkpointPool(guest.resources().sharedEffects) ==
              native.checkpointPool(host.resources().sharedEffects));
    }
    CHECK(otherDevice.texturesCreated == uploads);
}

TEST_CASE("combat missile resource identities use trusted tuples not live launch order",
          "[combat-resources]") {
    test::AttackCheckpointFixture source;
    const std::array resources{PlayerMissiles::Resource{17, &MissileSpec::of(0)},
                               PlayerMissiles::Resource{9, &MissileSpec::potion()},
                               PlayerMissiles::Resource{4, &MissileSpec::of(0)}};
    CombatResources host;
    REQUIRE(host.bind(source.device, {}, resources));
    REQUIRE(host.resources().missiles.size() == 2);
    CHECK(host.resources().missiles[0].id == 4);
    CHECK(host.resources().missiles[1].id == 9);
    PlayerMissiles native;
    PlayerMissiles replay;
    MissileLaunch launch;
    launch.owner = 3;
    launch.spec = &MissileSpec::of(0);
    launch.position = {0, 10, 0};
    REQUIRE(native.launch(launch));
    native.update(1.0f / 60, nullptr, {});
    const auto state =
        native.checkpoint(host.resources().missiles, host.resources().missileEffects);
    REQUIRE(state);
    REQUIRE(state->missiles.size() == 1);
    CHECK(state->missiles[0].resource == 4);
    REQUIRE(replay.restore(*state, host.resources().missiles, host.resources().missileEffects));
    CHECK(replay.checkpoint(host.resources().missiles, host.resources().missileEffects) == state);
    const MissileSpec unregistered{"OTHER"};
    launch.spec = &unregistered;
    REQUIRE(native.launch(launch));
    CHECK_FALSE(native.checkpoint(host.resources().missiles, host.resources().missileEffects));
    CHECK(native.count() == 2);
}

TEST_CASE("combat resource construction rejects malformed declarations atomically",
          "[combat-resources]") {
    const s32 invalid = GENERATE(0, 1, 2, 3, 4, 5);
    test::AttackCheckpointFixture source;
    CombatResources catalog;
    const std::array good{CombatResources::Bank{5, &source.weapons, {}}};
    REQUIRE(catalog.bind(source.device, good));
    const auto banks = catalog.bankCount();
    const auto effects = catalog.effectCount();
    auto malformed = std::vector<CombatResources::Bank>{good.begin(), good.end()};
    std::vector<PlayerMissiles::Resource> missiles;
    switch (invalid) {
    case 0: malformed[0].id = 0; break;
    case 1: malformed[0].id = 0x8000; break;
    case 2: malformed.push_back(malformed[0]); break;
    case 3: malformed[0].archive = nullptr; break;
    case 4: malformed[0].lenders.push_back(nullptr); break;
    case 5: missiles.push_back({1, nullptr}); break;
    default: break;
    }
    CHECK_FALSE(catalog.bind(source.device, malformed, missiles));
    CHECK(catalog.bankCount() == banks);
    CHECK(catalog.effectCount() == effects);
    REQUIRE(source.effects.start(source.device, source.weapons, "BURST", {}));
    CHECK(source.effects.checkpointPool(catalog.resources().sharedEffects));
}

TEST_CASE("combat potion artwork lookup is bounded and never binds resources",
          "[combat-resources]") {
    PlayerArsenal arsenal;
    CHECK(arsenal.potionModel(-1) == nullptr);
    CHECK(arsenal.potionModel(5) == nullptr);
    for (s32 kind = 0; kind <= 4; ++kind) {
        CHECK(arsenal.potionModel(kind) == nullptr);
    }
    CHECK(arsenal.missiles().count() == 0);
}

TEST_CASE("combat resources refuse uncatalogued trails without deleting native state",
          "[combat-resources]") {
    test::AttackCheckpointFixture source;
    CombatResources catalog;
    const std::array banks{CombatResources::Bank{7, &source.weapons, {}}};
    REQUIRE(catalog.bind(source.device, banks));
    const u32 effect = source.effects.startSet(source.device, source.weapons, "BURST", {}, {});
    REQUIRE(effect != 0);
    const auto slot = source.weapons.textures.find("SKIN");
    REQUIRE(slot);
    source.effects.attachTrail(effect, particleDescriptor(),
                               source.weapons.textures.texture(source.device, *slot));
    source.effects.update(0.1f);
    const auto native = source.effects.checkpoint(effect);
    REQUIRE(native);
    CHECK_FALSE(source.effects.checkpointPool(catalog.resources().sharedEffects));
    CHECK(source.effects.checkpoint(effect) == native);
    CHECK(source.effects.count() == 1);
}

struct NativeBindings {
    test::FakeRenderDevice device;
    LevelWorld world;
    ClassDataSet classes;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    WorldCollision openFlight;
    PlayerArsenal arsenal;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> party;
    CombatResources catalog;

    NativeBindings(const std::filesystem::path& root, s32 character) {
        REQUIRE(classes.load(root / "pdata"));
        REQUIRE(weapons.load(root / "WEAPONS"));
        CharacterSave save;
        save.character = character;
        save.color = 0;
        save.autoAttack = false;
        save.progress().inventory.potions.push_back(2);
        party[0].actor.spawn(3, save, classes.stats(character), {0, 20, 0}, 0);
        party[0].figure = PlayerFigure::load(device, root, save, false);
        REQUIRE(party[0].figure);
        const std::array lenders{&weapons.textures};
        effects.setTextureLenders(lenders);
        arsenal.bind({device, classes, weapons, openFlight, effects, audio, nullptr, {}}, lenders);
        for (s32 kind = 0; kind <= 4; ++kind) {
            REQUIRE(arsenal.potionModel(kind) != nullptr);
        }
        // Real selected-class/weapon assets; the empty world and collision keep
        // this a resource/continuation test, not a claim about scene admission.
        REQUIRE(
            catalog.bindStage(device, world, weapons, classes, party, arsenal, opponents, effects));
    }
    NativeBindings(const NativeBindings&) = delete;
    NativeBindings& operator=(const NativeBindings&) = delete;
    ~NativeBindings() {
        arsenal.clear();
        effects.clear();
        catalog.clear();
    }
};

TEST_CASE("combat stage catalogs bind independent native weapon potion and effect owners",
          "[combat-resources][assets]") {
    const s32 character = GENERATE(0, 2, 6);
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    NativeBindings native{root, character};
    NativeBindings replica{root, character};
    REQUIRE(native.catalog.matches(native.party));
    REQUIRE(replica.catalog.matches(replica.party));
    auto& player = native.party[0];
    native.arsenal.launchWeapon(player.actor, player.figure.get(), player.actor.facing(), 1, true);
    native.arsenal.throwPotion(player.actor, 0, 7);
    native.arsenal.burstPotion(2, {0, 20, 0}, 8, false);
    native.arsenal.missiles().update(1.0f / 60, nullptr, {});
    native.effects.update(1.0f / 60);
    const auto host = native.catalog.resources();
    const auto guest = replica.catalog.resources();
    const auto shots = native.arsenal.missiles().checkpoint(host.missiles, host.missileEffects);
    const auto effects = native.effects.checkpointPool(host.sharedEffects);
    REQUIRE(shots);
    REQUIRE(effects);
    REQUIRE(shots->missiles.size() == 2);
    REQUIRE_FALSE(shots->visuals.effects.empty());
    REQUIRE_FALSE(effects->effects.empty());
    const auto uploads = replica.device.texturesCreated;
    REQUIRE(replica.arsenal.missiles().restore(*shots, guest.missiles, guest.missileEffects));
    REQUIRE(replica.effects.restorePool(*effects, guest.sharedEffects));
    for (s32 tick = 0; tick < 220; ++tick) {
        native.arsenal.missiles().update(1.0f / 60, nullptr, {});
        replica.arsenal.missiles().update(1.0f / 60, nullptr, {});
        native.effects.update(1.0f / 60);
        replica.effects.update(1.0f / 60);
        CHECK(native.arsenal.missiles().checkpoint(host.missiles, host.missileEffects) ==
              replica.arsenal.missiles().checkpoint(guest.missiles, guest.missileEffects));
        CHECK(native.effects.checkpointPool(host.sharedEffects) ==
              replica.effects.checkpointPool(guest.sharedEffects));
    }
    CHECK(replica.device.texturesCreated == uploads);
    REQUIRE(native.arsenal.missiles().count() == 0);
    CHECK(native.effects.count() == 0);
    CHECK(native.effects.checkpointPool(host.sharedEffects));
    const auto previousFigure = std::move(player.figure);
    REQUIRE(previousFigure);
    player.figure = PlayerFigure::load(native.device, root, player.actor.save(), false);
    REQUIRE(player.figure);
    CHECK_FALSE(native.catalog.matches(native.party));
    native.catalog.clear(); // before releasing the retained old figure's archives
}
} // namespace
