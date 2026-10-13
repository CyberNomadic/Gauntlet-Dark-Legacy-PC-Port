#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/screens/ReplicaProjectiles.h"
#include "game/world/PlayerArsenal.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kStep = 1.0f / 60;
constexpr u64 kEpoch = 3;
constexpr u64 kReleaseTick = 11;

struct ParticleSample {
    CombatSnapshot state;
    std::map<u32, TreeParticles> native;
};

void checkVector(const Vec3& actual, const Vec3& expected) {
    CHECK(actual.x == Approx(expected.x).margin(0.00001f));
    CHECK(actual.y == Approx(expected.y).margin(0.00001f));
    CHECK(actual.z == Approx(expected.z).margin(0.00001f));
}

void checkParticles(const TreeParticles& actual, const TreeParticles& expected,
                    test::FakeRenderDevice& device, const CameraFrame& camera) {
    const auto& a = actual.field();
    const auto& b = expected.field();
    REQUIRE(a.size() == b.size());
    CHECK(a.particleCount() == b.particleCount());
    for (usize emitter = 0; emitter < b.size(); ++emitter) {
        CAPTURE(emitter);
        const auto& left = a.emitter(emitter);
        const auto& right = b.emitter(emitter);
        CHECK(left.phase() == right.phase());
        CHECK(left.age() == right.age());
        CHECK(left.spriteScale() == right.spriteScale());
        for (glm::length_t column = 0; column < 4; ++column) {
            for (glm::length_t row = 0; row < 4; ++row) {
                CHECK(left.node()[column][row] ==
                      Approx(right.node()[column][row]).margin(0.00001f));
            }
        }
        REQUIRE(left.particles().size() == right.particles().size());
        for (usize index = 0; index < right.particles().size(); ++index) {
            const auto& lp = left.particles()[index];
            const auto& rp = right.particles()[index];
            checkVector(lp.origin, rp.origin);
            checkVector(lp.velocity, rp.velocity);
            CHECK(lp.age == rp.age);
            checkVector(left.positionOf(lp), right.positionOf(rp));
        }
    }
    device.draws.clear();
    expected.draw(device, Mat4{1}, camera.right, camera.up, 1);
    const auto nativeDraws = device.draws;
    device.draws.clear();
    actual.draw(device, Mat4{1}, camera.right, camera.up, 1);
    REQUIRE(device.draws.size() == nativeDraws.size());
    for (usize draw = 0; draw < nativeDraws.size(); ++draw) {
        const auto& left = device.draws[draw];
        const auto& right = nativeDraws[draw];
        REQUIRE(left.texture != nullptr);
        REQUIRE(right.texture != nullptr);
        CHECK(left.texture->width() == right.texture->width());
        CHECK(left.texture->height() == right.texture->height());
        CHECK(left.state.blend == right.state.blend);
        CHECK(left.state.depthTest == right.state.depthTest);
        CHECK(left.state.depthWrite == right.state.depthWrite);
        CHECK(left.state.textureBlend == Approx(right.state.textureBlend).margin(0.00001f));
        REQUIRE(left.vertices.size() == right.vertices.size());
        CHECK(left.vertices.size() % 6 == 0);
        for (usize vertex = 0; vertex < right.vertices.size(); ++vertex) {
            checkVector(left.vertices[vertex].position, right.vertices[vertex].position);
            CHECK(left.vertices[vertex].color == right.vertices[vertex].color);
            CHECK(left.vertices[vertex].uv == right.vertices[vertex].uv);
        }
    }
}

struct ParticleFixture {
    test::FakeRenderDevice device;
    ItemArchive weapons;
    ItemArchive clientWeapons;
    ClassDataSet classes;
    WorldCollision collision;
    EffectTrees effects;
    LevelSoundscape audio;
    PlayerActor actor;
    std::unique_ptr<PlayerFigure> figure;
    std::unique_ptr<PlayerFigure> clientFigure;
    ProjectileResources host;
    ProjectileResources client;
    PlayerArsenal arsenal;
    const EnemyMissiles enemies;
    ShotIdentity origin;
    CameraFrame camera = CameraFrame::at({10, 20, 30});
    u32 tick = 0;

    explicit ParticleFixture(s32 character, u32 element) {
        const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
        REQUIRE(weapons.load(root / "WEAPONS"));
        REQUIRE(clientWeapons.load(root / "WEAPONS"));
        REQUIRE(classes.load(root / "pdata"));
        CharacterSave save;
        save.character = character;
        save.color = character % kColorCount;
        save.progress().experience = levelExperience(80);
        figure = PlayerFigure::load(device, root, save, false);
        clientFigure = PlayerFigure::load(device, root, save, false);
        REQUIRE(figure);
        REQUIRE(clientFigure);
        const std::array<TextureSet*, 1> lenders{&weapons.textures};
        const std::array<TextureSet*, 1> clientLenders{&clientWeapons.textures};
        const std::array<PlayerFigure*, InputCommand::kSeats> figures{nullptr, figure.get()};
        const std::array<PlayerFigure*, InputCommand::kSeats> clientFigures{nullptr,
                                                                            clientFigure.get()};
        REQUIRE(host.addPlayers(device, weapons, figures, lenders));
        REQUIRE(client.addPlayers(device, clientWeapons, clientFigures, clientLenders));
        effects.setTextureLenders(lenders);
        arsenal.bind({device, classes, weapons, collision, effects, audio, nullptr, {}}, lenders);
        actor.spawn(1, save, classes.stats(character), {7, 12, -9}, 0.4f);
        actor.save().progress().inventory.addPowerup(powerup::kWeapon, element, 0, 60);
        for (u32 animationTick = 0; animationTick < 180 && !figure->animator().released();
             ++animationTick) {
            figure->animate(0, 1, kStep, PlayerDeed::Attack);
        }
        REQUIRE(figure->animator().released());
        origin = {kEpoch, kReleaseTick, figure->animator().player().generation(), 4, 1};
        REQUIRE(origin.valid());
        arsenal.launchWeapon(actor, figure.get(), actor.facing(), 1, false);
        REQUIRE(arsenal.missiles().count() == 1);
        REQUIRE(arsenal.missiles().missile(0).release);
        REQUIRE(arsenal.missiles().visualHistory().size() == 1);
    }

    void step(std::span<const MissileTarget> targets = {}) {
        arsenal.missiles().update(kStep, nullptr, targets);
        ++tick;
    }

    ParticleSample capture() {
        // Refresh the native draw-only sprite frame before retaining our independent oracle.
        arsenal.missiles().visuals().draw(device, Mat4{1}, {}, &camera, 1);
        ParticleSample sample;
        sample.state.motion.epoch = kEpoch;
        sample.state.motion.tick = kReleaseTick + tick;
        sample.state.motion.cameraContinuity = 1;
        sample.state.motion.players[origin.seat] = SeatMotion{origin.grant, 1};
        sample.state.players[origin.seat] = PlayerCombatState{};
        REQUIRE(ProjectileCapture::append(sample.state, host, arsenal.missiles(), enemies));
        for (auto& shot : sample.state.projectiles) {
            if (!shot.particles) {
                continue;
            }
            // PlayReplication adds session ownership after the native capture; do the same
            // here without substituting the flight history or native particle simulation.
            shot.particles->origin = origin;
            const auto& visuals = arsenal.missiles().visuals();
            const EffectTrees::Effect* native = nullptr;
            for (usize index = 0; index < visuals.count(); ++index) {
                if (visuals.effect(index).instance == shot.instance) {
                    native = &visuals.effect(index);
                    break;
                }
            }
            REQUIRE(native != nullptr);
            REQUIRE(sample.native.emplace(shot.resource, native->particles).second);
            CHECK(shot.particles->flight.retiring == native->retiring);
        }
        return sample;
    }

    usize verify(const ParticleSample& sample, ReplicaProjectiles& replica) {
        const auto packet = CombatPacket::encode(sample.state);
        REQUIRE(packet);
        const auto received = CombatPacket::decode(*packet);
        REQUIRE(received);
        const auto textures = device.texturesCreated;
        REQUIRE(replica.show(*received, client));
        REQUIRE(replica.count() == sample.state.projectiles.size());
        device.draws.clear();
        replica.draw(device, client, Mat4{1}, {}, camera);
        const auto rendered = device.draws;
        usize particles = 0;
        for (const auto& shot : received->projectiles) {
            if (!shot.particles) {
                CHECK(replica.particles(shot) == nullptr);
                continue;
            }
            const auto found = sample.native.find(shot.resource);
            REQUIRE(found != sample.native.end());
            const auto* replayed = replica.particles(shot);
            REQUIRE(replayed != nullptr);
            checkParticles(*replayed, found->second, device, camera);
            particles += found->second.field().particleCount();
            // The actual replica draw must contain the same quad batches, not merely a
            // populated diagnostic particle field hidden from the renderer.
            for (const auto& quadBatch : device.draws) {
                CHECK(std::ranges::any_of(rendered, [&](const auto& draw) {
                    return draw.texture == quadBatch.texture &&
                           draw.vertices == quadBatch.vertices && draw.state == quadBatch.state;
                }));
            }
        }
        CHECK(device.texturesCreated == textures);
        return particles;
    }
};

TEST_CASE("elemental guest particles reconstruct delayed native history and replay corrections",
          "[netplay][replica-particles][assets]") {
    const s32 character = GENERATE(0, 2, 6);
    const u32 element = GENERATE(1U, 2U, 3U, 4U);
    CAPTURE(character, element);
    ParticleFixture f(character, element);
    std::vector<ParticleSample> history;
    for (u32 tick = 1; tick <= 40; ++tick) {
        f.step();
        if (tick == 8 || tick == 24 || tick == 40) {
            history.push_back(f.capture());
        }
    }
    REQUIRE(history.size() == 3);
    REQUIRE_FALSE(history[1].native.empty());
    ReplicaProjectiles replica;
    REQUIRE(replica.begin(kEpoch));
    usize visible = f.verify(history[1], replica); // first receipt starts well after emission
    visible += f.verify(history[1], replica);      // repeated rendering cannot emit twice
    visible += f.verify(history[2], replica);      // missing intermediate snapshots
    CHECK(visible > 0);

    // A newer authority packet may replace speculative history with a shorter history.
    auto corrected = history[0];
    corrected.state.motion.tick = 100;
    f.verify(corrected, replica);
    auto resumed = history[1];
    resumed.state.motion.tick = 101;
    for (auto& shot : resumed.state.projectiles) {
        shot.instance += 1000; // local predicted pool IDs need not match host pool IDs
    }
    f.verify(resumed, replica);
    CHECK_FALSE(replica.show(history[0].state, f.client)); // stale transport receipt
    f.verify(resumed, replica);

    // Losing the release removes its emitter and particles; a new epoch clears every key.
    CombatSnapshot empty;
    empty.motion = resumed.state.motion;
    empty.players = resumed.state.players;
    ++empty.motion.tick;
    REQUIRE(replica.show(empty, f.client));
    for (const auto& shot : resumed.state.projectiles) {
        CHECK(replica.particles(shot) == nullptr);
    }
    REQUIRE(replica.begin(kEpoch + 1));
    CHECK(replica.count() == 0);
    CHECK_FALSE(replica.show(resumed.state, f.client));
}

TEST_CASE("elemental projectile impact and lifetime retain only native particle tails",
          "[netplay][replica-particles][assets]") {
    const s32 character = GENERATE(0, 2, 6);
    const u32 element = GENERATE(1U, 2U, 3U, 4U);
    const bool impact = GENERATE(false, true);
    CAPTURE(character, element, impact);
    ParticleFixture f(character, element);
    ReplicaProjectiles replica;
    REQUIRE(replica.begin(kEpoch));
    bool retired = false;
    bool sawTail = false;
    usize particles = 0;
    for (u32 tick = 1; tick <= 300; ++tick) {
        if (impact && tick == 20) {
            REQUIRE(f.arsenal.missiles().count() == 1);
            const auto& missile = f.arsenal.missiles().missile(0);
            // Contact lies inside the next tick's sweep, not at its free-flight endpoint.
            const Vec3 ahead = missile.position + missile.velocity * (kStep * 0.3f);
            const std::array<MissileTarget, 1> targets{
                MissileTarget{7, ahead - Vec3{0, 1, 0}, 0.15f, 2}};
            f.step(targets);
            CHECK(f.arsenal.missiles().count() == 0);
        } else {
            f.step();
        }
        const bool nowRetired = f.arsenal.missiles().count() == 0;
        if (tick == 18 || (nowRetired && (!retired || tick % 4 == 0))) {
            const auto sample = f.capture();
            particles += f.verify(sample, replica);
            if (nowRetired) {
                for (const auto& shot : sample.state.projectiles) {
                    REQUIRE(shot.particles);
                    CHECK(shot.particles->flight.retiring);
                    CHECK_FALSE(shot.rollback);
                    CHECK(shot.source != ProjectileSource::Player);
                    CHECK(shot.source != ProjectileSource::Streak);
                    sawTail =
                        sawTail || sample.native.at(shot.resource).field().particleCount() > 0;
                }
            }
        }
        retired = nowRetired;
        if (retired && f.arsenal.missiles().visuals().count() == 0) {
            const auto empty = f.capture();
            CHECK(empty.state.projectiles.empty());
            f.verify(empty, replica);
            CHECK(replica.count() == 0);
            break;
        }
    }
    CHECK(retired);
    CHECK(sawTail);
    CHECK(particles > 0);
    CHECK(f.arsenal.missiles().visuals().count() == 0);
}
} // namespace
