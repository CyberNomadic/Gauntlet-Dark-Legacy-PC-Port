#include <algorithm>
#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/screens/ReplicaProjectiles.h"
#include "game/world/PotionEmission.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr f32 kStep = 1.0f / 60;
constexpr u64 kEpoch = 7;
constexpr u64 kBirthTick = 11;

struct Sample {
    CombatSnapshot snapshot;
    std::vector<TreeParticles> native;
};

void checkTexture(const Texture* actual, const Texture* expected) {
    REQUIRE((actual != nullptr) == (expected != nullptr));
    if (actual == nullptr) {
        return;
    }
    const auto* actualTexture = dynamic_cast<const test::FakeTexture*>(actual);
    const auto* expectedTexture = dynamic_cast<const test::FakeTexture*>(expected);
    REQUIRE(actualTexture != nullptr);
    REQUIRE(expectedTexture != nullptr);
    CHECK(actualTexture->width() == expectedTexture->width());
    CHECK(actualTexture->height() == expectedTexture->height());
    CHECK((actualTexture->pixels == expectedTexture->pixels));
}

void checkDrawState(DrawState actual, const DrawState& expected) {
    // Independent archive loads produce different GPU texture objects, including
    // POT_BLU_TW's blended next frame. Compare their actual images before mapping
    // the three bindings; retain exact comparison of every scalar render state.
    checkTexture(actual.lightmap, expected.lightmap);
    checkTexture(actual.maskedTexture, expected.maskedTexture);
    checkTexture(actual.nextTexture, expected.nextTexture);
    actual.lightmap = expected.lightmap;
    actual.maskedTexture = expected.maskedTexture;
    actual.nextTexture = expected.nextTexture;
    CAPTURE(actual.blend, expected.blend, actual.textureBlend, expected.textureBlend,
            actual.uvScale.x, expected.uvScale.x, actual.uvScale.y, expected.uvScale.y,
            actual.uvOffset.x, expected.uvOffset.x, actual.uvOffset.y, expected.uvOffset.y,
            actual.alphaTest, expected.alphaTest, actual.cullBack, expected.cullBack,
            actual.depthWrite, expected.depthWrite, actual.depthTest, expected.depthTest,
            actual.mipmaps, expected.mipmaps, actual.clampTexture, expected.clampTexture,
            actual.alphaToCoverage, expected.alphaToCoverage, actual.smoothSprite,
            expected.smoothSprite, actual.darken, expected.darken, actual.colorScale,
            expected.colorScale);
    CHECK(actual == expected);
}

struct Fixture {
    test::FakeRenderDevice device;
    ItemArchive hostArchive;
    ItemArchive guestArchive;
    ProjectileResources hostResources;
    ProjectileResources guestResources;
    PlayerMissiles native;
    EnemyMissiles enemies;
    ReplicaProjectiles replica;
    CameraFrame camera = CameraFrame::at({10, 20, 30});
    PotionIdentity origin{kEpoch, kBirthTick, 3, 4, 1, PotionIdentity::Kind::Throw};
    u64 ticks = 0;
    s32 kind = 0;

    explicit Fixture(s32 color) : kind(color) {
        const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path();
        REQUIRE(hostArchive.load(root));
        REQUIRE(guestArchive.load(root));
        for (s32 potion = 1; potion <= 4; ++potion) {
            const auto name = PotionEmission::look(potion).bottle;
            const auto id = static_cast<u32>(100 + potion);
            REQUIRE(hostResources.addTree(id, hostArchive, name, device));
            REQUIRE(guestResources.addTree(id, guestArchive, name, device));
            REQUIRE(guestResources.hasParticles(id));
            const auto tree = hostArchive.trees.find(name);
            REQUIRE(tree);
            REQUIRE_FALSE(hostArchive.trees.tree(*tree).sequences.empty());
            CHECK(hostArchive.trees.tree(*tree).sequences.front().frames == 0);
        }
        native.bindVisuals(device);
        auto launch = PotionEmission::throwPotion({1, {7, 12, -9}, {0, 0, 1}, kind, 40, 1, 0}, 12);
        launch.archive = &hostArchive;
        launch.potionRelease = MissilePotionRelease{origin.generation};
        REQUIRE(native.launch(launch));
        REQUIRE(native.count() == 1);
        REQUIRE(native.visualHistory().size() == 1);
        CHECK(native.visualHistory().front().potion == kind);
        REQUIRE(replica.begin(kEpoch));
    }

    void step(std::span<const MissileTarget> targets = {}) {
        native.update(kStep, nullptr, targets);
        ++ticks;
    }

    Sample capture(bool held = false) {
        native.visuals().draw(device, Mat4{1}, {}, &camera, held ? -1.0f : 1.0f);
        Sample sample;
        sample.snapshot.motion.epoch = kEpoch;
        sample.snapshot.motion.tick = kBirthTick + ticks;
        sample.snapshot.motion.cameraContinuity = 1;
        sample.snapshot.motion.players[origin.seat] = SeatMotion{origin.grant, 1};
        sample.snapshot.players[origin.seat] = PlayerCombatState{};
        REQUIRE(ProjectileCapture::append(sample.snapshot, hostResources, native, enemies));
        for (auto& shot : sample.snapshot.projectiles) {
            REQUIRE(shot.particles);
            REQUIRE(shot.source == ProjectileSource::PlayerEffect);
            CHECK(shot.particles->potion == kind);
            CHECK(shot.particles->character == 0);
            CHECK_FALSE(shot.particles->origin);
            shot.particles->potionOrigin = origin;
            shot.particles->held = held;
            const EffectTrees::Effect* effect = nullptr;
            for (usize i = 0; i < native.visuals().count(); ++i) {
                if (native.visuals().effect(i).instance == shot.instance) {
                    effect = &native.visuals().effect(i);
                    break;
                }
            }
            REQUIRE(effect != nullptr);
            CHECK(effect->retiring == shot.particles->flight.retiring);
            sample.native.push_back(effect->particles);
        }
        REQUIRE(sample.snapshot.valid());
        return sample;
    }

    usize verify(const Sample& sample) {
        const auto packet = CombatPacket::encode(sample.snapshot);
        REQUIRE(packet);
        const auto decoded = CombatPacket::decode(*packet);
        REQUIRE(decoded);
        const usize uploads = device.texturesCreated;
        const usize physical = native.count();
        const usize effects = native.visuals().count();
        REQUIRE(replica.show(*decoded, guestResources));
        device.draws.clear();
        replica.draw(device, guestResources, Mat4{1}, {}, camera);
        const auto rendered = device.draws;
        REQUIRE(decoded->projectiles.size() == sample.native.size());
        usize count = 0;
        for (usize index = 0; index < decoded->projectiles.size(); ++index) {
            const auto& shot = decoded->projectiles[index];
            CAPTURE(sample.snapshot.motion.tick, shot.textureFrame, shot.particles->held);
            const auto* actual = replica.particles(shot);
            REQUIRE(actual != nullptr);
            const auto& expected = sample.native[index];
            // Includes every emitter's RNG, clocks, live particle positions,
            // velocity, age and node history, not just a mesh/draw count.
            CHECK(actual->checkpoint() == expected.checkpoint());
            count += expected.field().particleCount();
            const f32 blend = shot.particles->held ? -1.0f : 1.0f;
            device.draws.clear();
            expected.draw(device, Mat4{1}, camera.right, camera.up, blend);
            const auto oracle = device.draws;
            device.draws.clear();
            actual->draw(device, Mat4{1}, camera.right, camera.up, blend);
            REQUIRE(device.draws.size() == oracle.size());
            for (usize draw = 0; draw < oracle.size(); ++draw) {
                CHECK(device.draws[draw].vertices == oracle[draw].vertices);
                checkTexture(device.draws[draw].texture, oracle[draw].texture);
                checkDrawState(device.draws[draw].state, oracle[draw].state);
                CHECK(std::ranges::any_of(rendered, [&](const auto& batch) {
                    return batch.texture == device.draws[draw].texture &&
                           batch.vertices == device.draws[draw].vertices &&
                           batch.state == device.draws[draw].state;
                }));
            }
        }
        CHECK(device.texturesCreated == uploads);
        CHECK(native.count() == physical);
        CHECK(native.visuals().count() == effects);
        // The replica borrows only trusted visual resources: no sound player,
        // collision dispatcher, inventory or native projectile pool is supplied.
        return count;
    }
};

TEST_CASE("native thrown potion particles survive delayed receipt held frames and corrections",
          "[netplay][replica-potion-particles][assets]") {
    const s32 kind = GENERATE(1, 2, 3, 4);
    CAPTURE(kind);
    Fixture f(kind);
    std::vector<Sample> samples;
    for (u32 tick = 1; tick <= 40; ++tick) {
        f.step();
        if (tick == 8 || tick == 24 || tick == 40) {
            samples.push_back(f.capture());
        }
    }
    REQUIRE(samples.size() == 3);
    REQUIRE_FALSE(samples[1].native.empty());
    CHECK(f.verify(samples[1]) > 0);
    f.verify(samples[1]);
    CHECK(f.verify(samples[2]) > 0);
    auto held = f.capture(true);
    held.snapshot.motion.tick = 90;
    f.verify(held);
    f.verify(held);
    auto correction = samples[0];
    correction.snapshot.motion.tick = 100;
    f.verify(correction);
    auto continued = samples[1];
    continued.snapshot.motion.tick = 101;
    for (auto& shot : continued.snapshot.projectiles) {
        shot.instance += 500; // private pool IDs do not define release identity
    }
    f.verify(continued);
    CHECK_FALSE(f.replica.show(samples[0].snapshot, f.guestResources));
    f.verify(continued);
    auto replaced = continued;
    ++replaced.snapshot.motion.tick;
    for (auto& shot : replaced.snapshot.projectiles) {
        ++shot.particles->potionOrigin->generation;
        ++shot.continuity;
    }
    f.verify(replaced);
    for (const auto& shot : continued.snapshot.projectiles) {
        CHECK(f.replica.particles(shot) == nullptr);
    }
    REQUIRE(f.replica.begin(kEpoch + 1));
    CHECK(f.replica.count() == 0);
    CHECK_FALSE(f.replica.show(replaced.snapshot, f.guestResources));
}

TEST_CASE("native potion collision and lifetime preserve exact retiring particle tails",
          "[netplay][replica-potion-particles][assets]") {
    const s32 kind = GENERATE(1, 2, 3, 4);
    const bool impact = GENERATE(false, true);
    CAPTURE(kind, impact);
    Fixture f(kind);
    bool retired = false;
    bool visibleTail = false;
    for (u32 tick = 1; tick <= 300; ++tick) {
        if (impact && tick == 20) {
            REQUIRE(f.native.count() == 1);
            const auto& missile = f.native.missile(0);
            const Vec3 near = missile.position + missile.velocity * (kStep * 0.3f);
            const std::array targets{MissileTarget{7, near - Vec3{0, 1, 0}, 0.15f, 2}};
            f.step(targets);
            CHECK(f.native.count() == 0);
        } else {
            f.step();
        }
        const bool nowRetired = f.native.count() == 0;
        if (tick == 18 || (nowRetired && (!retired || tick % 4 == 0))) {
            const auto sample = f.capture();
            const usize count = f.verify(sample);
            if (nowRetired) {
                visibleTail |= count > 0;
                for (const auto& shot : sample.snapshot.projectiles) {
                    CHECK(shot.particles->flight.retiring);
                    CHECK_FALSE(shot.potion);
                    CHECK_FALSE(shot.rollback);
                }
                // Late first receipt reconstructs the same terminal emitter,
                // including a collision inside the final physics substep.
                if (!retired) {
                    f.replica.clear();
                    REQUIRE(f.replica.begin(kEpoch));
                    f.verify(sample);
                }
            }
        }
        retired = nowRetired;
        if (retired && f.native.visuals().count() == 0) {
            const auto empty = f.capture();
            CHECK(empty.snapshot.projectiles.empty());
            f.verify(empty);
            break;
        }
    }
    CHECK(retired);
    CHECK(visibleTail);
    CHECK(f.native.visuals().count() == 0);
    CHECK(f.replica.count() == 0);
}
} // namespace
