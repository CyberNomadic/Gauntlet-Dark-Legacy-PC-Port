#include <algorithm>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/CombatCapture.h"
#include "game/screens/FireShieldPresentation.h"
#include "game/screens/PartyFigures.h"
#include "game/screens/ReplicaActors.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct ShieldStock {
    std::filesystem::path root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    ItemArchive weapons;
    ShieldStock() {
        REQUIRE(powerups.load(root / "POWERUPS"));
        REQUIRE(weapons.load(root / "WEAPONS"));
    }
};

void sameShieldGeometry(std::span<const test::RecordedDraw> actual,
                        std::span<const test::RecordedDraw> expected) {
    REQUIRE_FALSE(expected.empty());
    REQUIRE(actual.size() == expected.size());
    for (usize i = 0; i < actual.size(); ++i) {
        CAPTURE(i);
        const auto& a = actual[i];
        const auto& b = expected[i];
        REQUIRE(a.vertices.size() == b.vertices.size());
        CHECK(a.state.depthWrite == b.state.depthWrite);
        CHECK(a.state.depthTest == b.state.depthTest);
        CHECK(a.state.alphaTest == b.state.alphaTest);
        CHECK(a.blend() == b.blend());
        for (usize n = 0; n < a.vertices.size(); ++n) {
            const auto av = a.transform * Vec4{a.vertices[n].position, 1};
            const auto bv = b.transform * Vec4{b.vertices[n].position, 1};
            for (s32 axis = 0; axis < 3; ++axis) {
                CHECK(av[axis] == Approx(bv[axis]).margin(0.0001f));
            }
            CHECK(a.vertices[n].color == b.vertices[n].color);
            CHECK(a.vertices[n].uv.x == Approx(b.vertices[n].uv.x).margin(0.0001f));
            CHECK(a.vertices[n].uv.y == Approx(b.vertices[n].uv.y).margin(0.0001f));
        }
    }
}

void nativeArm(ShieldStock& stock, PlayerFigure& figure, u32 equipment) {
    const bool left = (equipment & powerup::kLeftGauntlet) != 0;
    figure.holdOnArm(stock.device, left ? &stock.powerups : &stock.weapons,
                     left ? "BOSSGAUNTL" : PartyFigures::shieldObjectOf(equipment));
}

TEST_CASE("replicated arm equipment matches native shield priority and costume hiding",
          "[netplay][replica-shields][assets]") {
    ShieldStock stock;
    const auto camera = CameraFrame::at({15, 20, 30});
    const Mat4 body = glm::rotate(glm::translate(Mat4{1}, Vec3{3, 2, 8}), 0.4f, Vec3{0, 1, 0});
    constexpr std::array kEquipment{0U,
                                    powerup::kReflectShield,
                                    powerup::kFireShield,
                                    powerup::kLightningShield,
                                    powerup::kShields,
                                    powerup::kFireShield | powerup::kLightningShield,
                                    powerup::kLeftGauntlet | powerup::kShields};
    for (s32 character = 0; character < 16; ++character) {
        CharacterSave save;
        save.character = character;
        auto figure = PlayerFigure::load(stock.device, stock.root, save, false);
        REQUIRE(figure);
        REQUIRE(figure->bindArmEquipment(stock.device, stock.powerups, stock.weapons));
        CHECK_FALSE(figure->armEquipmentReady(powerup::kRightGauntlet));
        for (const auto equipment : kEquipment) {
            for (const f32 magnitude : {0.0f, 1.0f}) {
                CAPTURE(character, equipment, magnitude);
                nativeArm(stock, *figure, equipment);
                figure->setShielded((equipment & powerup::kShields) != 0);
                figure->animate(magnitude, 2, 1.0f / 30);
                TreePose pose;
                figure->animator().evaluatePresentation(pose, 1);
                stock.device.draws.clear();
                figure->draw(stock.device, Mat4{1}, body, {}, 1, false, &camera, 1);
                const auto expected = stock.device.draws;
                const auto textures = stock.device.texturesCreated;
                const auto revision = figure->animationRevision();
                const auto checkpoint = figure->animator().checkpoint();
                for (s32 repeat = 0; repeat < 2; ++repeat) {
                    stock.device.draws.clear();
                    figure->drawPose(stock.device, Mat4{1}, body, {}, pose,
                                     figure->animator().action(),
                                     static_cast<f32>(figure->animationRevision() - 1), nullptr,
                                     &camera, TreeModel::Pass::All, false, equipment);
                    sameShieldGeometry(stock.device.draws, expected);
                }
                CHECK(stock.device.texturesCreated == textures);
                CHECK(figure->animationRevision() == revision);
                CHECK(figure->animator().checkpoint() == checkpoint);
                // External rendering must not replace the native equipment or its hiding.
                stock.device.draws.clear();
                figure->draw(stock.device, Mat4{1}, body, {}, 1, false, &camera, 1);
                sameShieldGeometry(stock.device.draws, expected);
            }
        }
    }
}

TEST_CASE("replica shields switch correct and hold using only preloaded resources",
          "[netplay][replica-shields][assets]") {
    ShieldStock stock;
    CharacterSave save;
    save.character = 7;
    PlayerRuntime host;
    host.actor.spawn(0, save, nullptr, {3, 0, 7}, 0.6f);
    host.figure = PlayerFigure::load(stock.device, stock.root, save, false);
    REQUIRE(host.figure);
    ReplicaActors guest;
    REQUIRE(guest.begin(1));
    REQUIRE(guest.setPlayer(0, 1, PlayerFigure::load(stock.device, stock.root, save, false)));
    Enemies enemies;
    MotionSnapshot motion;
    motion.epoch = motion.cameraContinuity = 1;
    motion.players[0] = SeatMotion{1, 1, host.actor.position(), host.actor.yaw()};
    auto captured = CombatCapture::capture(motion, std::span{&host, 1}, enemies);
    REQUIRE(captured);
    captured->players[0]->armEquipment = powerup::kFireShield;
    CHECK_FALSE(guest.show(*captured, enemies));
    REQUIRE(guest.bindCompanions(stock.device, stock.powerups, &stock.weapons));
    REQUIRE(guest.fireShieldTree(0));
    CHECK(guest.fireShieldTree(4) == nullptr);
    const auto textures = stock.device.texturesCreated;
    const auto camera = CameraFrame::at({15, 20, 30});
    for (const auto equipment :
         {powerup::kFireShield, powerup::kReflectShield, 0U,
          powerup::kLeftGauntlet | powerup::kShields, powerup::kLightningShield, 0U}) {
        CAPTURE(equipment);
        nativeArm(stock, *host.figure, equipment);
        captured->players[0]->armEquipment = equipment;
        REQUIRE(guest.show(*captured, enemies));
        stock.device.draws.clear();
        host.figure->draw(stock.device, Mat4{1}, host.actor.transform(), {}, 1, false, &camera, 1);
        const auto expected = stock.device.draws;
        for (s32 held = 0; held < 3; ++held) {
            stock.device.draws.clear();
            REQUIRE(guest.show(*captured, enemies));
            guest.draw(stock.device, enemies, Mat4{1}, {}, camera, 0, nullptr);
            sameShieldGeometry(stock.device.draws, expected);
        }
        CHECK(stock.device.texturesCreated == textures);
    }
    auto malformed = *captured;
    malformed.players[0]->armEquipment = powerup::kRightGauntlet;
    CHECK_FALSE(guest.show(malformed, enemies));
    CHECK(guest.visiblePlayers() == 1);
}

TEST_CASE("the native fire shield blaze is an authored mesh loop with no particle emitters",
          "[netplay][replica-shields][assets]") {
    ShieldStock stock;
    CompanionResources resources;
    REQUIRE(resources.bindPowerups(stock.device, stock.powerups, &stock.weapons));
    const auto* tree = resources.fireShieldTree();
    REQUIRE(tree);
    CHECK(tree->name == "FW_SHLD_ACTIVE");
    REQUIRE(tree->sequences.size() == 1);
    CHECK(tree->sequences[0].name == "ACTIVE");
    CHECK(tree->sequences[0].frames > 0);
    CHECK(std::ranges::none_of(tree->nodes, [](const auto& node) { return node.particle >= 0; }));
    CHECK(std::ranges::any_of(tree->nodes, [&](const auto& node) {
        return stock.weapons.models.find(node.object).has_value();
    }));
}

TEST_CASE("fire shield presentation restores the native phase through loops stops and corrections",
          "[netplay][replica-shields][shield-prediction][assets]") {
    ShieldStock stock;
    CompanionResources resources;
    REQUIRE(resources.bindPowerups(stock.device, stock.powerups, &stock.weapons));
    const auto* tree = resources.fireShieldTree();
    REQUIRE(tree);
    PowerupCompanion host;
    host.choose(stock.device, PowerupCompanion::Kind::FireShield, stock.powerups, &stock.weapons);
    for (s32 tick = 0; tick < 113; ++tick) {
        host.update(1.0f / 60, PlayerAnimator::Action::ShieldRun, false, false);
    }
    const Mat4 body = glm::rotate(glm::translate(Mat4{1}, Vec3{3, 2, 8}), 0.4f, Vec3{0, 1, 0});
    const auto seedOf = [&]() -> std::optional<CompanionState> {
        const auto visual = host.visual(body, 1);
        if (!visual) {
            return std::nullopt;
        }
        return CompanionState{visual->form,
                              visual->placement,
                              {0, visual->sequence, visual->generation, visual->frame, 1},
                              visual->textureClock,
                              visual->alpha,
                              host.animationCheckpoint()};
    };
    FireShieldPresentation replay;
    REQUIRE(replay.restore(*tree, seedOf()));
    const auto camera = CameraFrame::at({15, 20, 30});
    const auto textures = stock.device.texturesCreated;
    for (s32 tick = 0; tick < 200; ++tick) {
        const bool running = tick < 70 || (tick >= 85 && tick < 180);
        const f32 dt = tick % 17 == 0 ? 0 : 1.0f / 60;
        const f32 alpha = std::clamp(static_cast<f32>(200 - tick) / 60, 0.1f, 1.0f);
        CAPTURE(tick, running, dt);
        if (tick == 45 || tick == 165) {
            REQUIRE(replay.restore(*tree, seedOf()));
        }
        host.choose(stock.device,
                    running ? PowerupCompanion::Kind::FireShield : PowerupCompanion::Kind::None,
                    stock.powerups, &stock.weapons);
        host.update(dt, PlayerAnimator::Action::ShieldRun, false, false);
        REQUIRE(replay.step(running, dt, alpha));
        const auto actual = replay.sample(body);
        const auto expected = host.visual(body, alpha);
        REQUIRE(actual.has_value() == expected.has_value());
        if (actual) {
            REQUIRE(resources.accepts(1, *actual));
            CHECK(actual->rollback == std::optional{host.animationCheckpoint()});
            CHECK(actual->animation.generation == expected->generation);
            CHECK(actual->animation.frame == expected->frame);
            CHECK(actual->textureClock == expected->textureClock);
            CHECK(actual->placement == expected->placement);
            CHECK(actual->alpha == expected->alpha);
            stock.device.draws.clear();
            host.draw(stock.device, Mat4{1}, body, {}, alpha, &camera, 1);
            const auto geometry = stock.device.draws;
            stock.device.draws.clear();
            resources.draw(stock.device, 1, *actual, Mat4{1}, {}, camera, TreeModel::Pass::All);
            sameShieldGeometry(stock.device.draws, geometry);
            CHECK(stock.device.texturesCreated == textures);
            CHECK(replay.sample(body)->rollback == actual->rollback);
            auto invalid = actual;
            invalid->rollback->sequence = 99;
            CHECK_FALSE(replay.restore(*tree, invalid));
            CHECK(replay.sample(body)->rollback == actual->rollback);
        }
    }
    REQUIRE(replay.restore(*tree, std::nullopt));
    CHECK_FALSE(replay.sample(body));
    REQUIRE(replay.step(true, 1.0f / 60, 1));
    REQUIRE(replay.sample(body));
    CHECK(replay.sample(body)->textureClock == 0.5f);
    CHECK(replay.sample(body)->animation.generation != 0);
}
} // namespace
