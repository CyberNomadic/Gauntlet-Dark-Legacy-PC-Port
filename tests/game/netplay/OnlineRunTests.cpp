#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/SoundPlayer.h"
#include "engine/platform/Paths.h"
#include "engine/platform/Window.h"
#include "engine/render/ThreadedRenderDevice.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/app/OnlineRun.h"
#include "game/screens/GameContext.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void exerciseLobby(RenderDevice& hostDevice, RenderDevice& guestDevice,
                   const std::function<void()>& frameFinished) {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const GameConfig config;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    GameContext context;
    context.config = &config;
    context.levels = &levels;
    context.unpackedRoot = root;
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    context.sounds = &sounds;
    context.movieMixer = &mixer;
    OnlineRun host;
    OnlineRun guest;
    REQUIRE(host.openLobby(hostDevice, context, {}, "test", std::string(64, 'a'), 1, true));
    const auto pump = [&](const auto& finished) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!finished() && std::chrono::steady_clock::now() < deadline) {
            host.update({}, {});
            guest.update({}, {});
            REQUIRE(hostDevice.beginFrame());
            REQUIRE(guestDevice.beginFrame());
            host.render(hostDevice, Mat4{1}, 640, 448, 1.0 / 60, 1);
            guest.render(guestDevice, Mat4{1}, 640, 448, 1.0 / 60, 1);
            hostDevice.endFrame();
            guestDevice.endFrame();
            hostDevice.waitIdle();
            guestDevice.waitIdle();
            frameFinished();
            std::array<f32, 1600> audio{};
            sounds.update();
            mixer.mix(audio);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(finished());
    };
    pump([&] { return !host.lobby().invitation.empty(); });
    CHECK_FALSE(host.lobby().selected);
    CHECK_FALSE(host.lobby().canStart);
    REQUIRE_FALSE(host.start());
    REQUIRE(guest.openLobby(guestDevice, context, host.lobby().invitation, "test",
                            std::string(64, 'a'), 1, true));
    pump([&] { return guest.lobby().connected && host.lobby().players == 2; });
    CHECK_FALSE(host.ready(true));
    PartyMember first;
    first.player = 0;
    first.save.name = "HOST";
    first.save.progress().health = 1000;
    auto second = first;
    second.player = 2;
    second.save.name = "GUEST";
    second.save.color = 1;
    REQUIRE(host.select(std::array{first}));
    REQUIRE(guest.select(std::array{second}));
    REQUIRE(host.settings({2, 2, 1}));
    pump([&] { return guest.lobby().settings == RoomSettings{2, 2, 1}; });
    REQUIRE(host.ready(true));
    REQUIRE(guest.ready(true));
    pump([&] { return host.lobby().canStart; });
    REQUIRE(host.start());
    pump([&] {
        return host.playing() && guest.playing() && host.status().empty() && guest.status().empty();
    });
    REQUIRE(host.pause(0));
    for (s32 tick = 0; tick < 120; ++tick) {
        host.update({}, {});
        guest.update({}, {});
    }
    CHECK(host.status() == "Local menu open - the game continues for everyone.");
    CHECK(guest.status().empty());
    host.resume();
    host.update({}, {});
    CHECK(host.status().empty());
    REQUIRE(guest.pause(2));
    guest.update({}, {});
    CHECK(guest.status() == "Local menu open - the game continues for everyone.");
    guest.resume();
    guest.update({}, {});
    CHECK(guest.status().empty());
    hostDevice.waitIdle();
    guestDevice.waitIdle();
}

TEST_CASE("menu-created iroh rooms select characters ready and enter the shared tower",
          "[netplay][online-lobby][assets]") {
    const bool threaded = GENERATE(false, true);
    auto hostBackend = std::make_unique<test::FakeRenderDevice>();
    auto guestBackend = std::make_unique<test::FakeRenderDevice>();
    auto& host = *hostBackend;
    auto& guest = *guestBackend;
    std::unique_ptr<RenderDevice> hostRenderer = std::move(hostBackend);
    std::unique_ptr<RenderDevice> guestRenderer = std::move(guestBackend);
    if (threaded) {
        hostRenderer =
            std::make_unique<ThreadedRenderDevice>(std::move(hostRenderer), Extent2D{640, 448});
        guestRenderer =
            std::make_unique<ThreadedRenderDevice>(std::move(guestRenderer), Extent2D{640, 448});
    }
    exerciseLobby(*hostRenderer, *guestRenderer, [&] {
        host.draws.clear();
        guest.draws.clear();
    });
    // Clearing the loading or pause notice must remove its texture, not upload
    // the zero-sized image returned by the font renderer for empty text.
    CHECK(host.emptyTexturesCreated == 0);
    CHECK(guest.emptyTexturesCreated == 0);
}

TEST_CASE("online lobby starts and resumes on host and guest Vulkan renderers",
          "[netplay][online-lobby-gpu][gpu][assets]") {
    (void)test::assetOrSkip("WEAPONS/ANIM.PS2");
    const bool threaded = GENERATE(false, true);
    REQUIRE(glfwInit() == GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    auto hostWindow = createGlfwWindow({.title = "Online host test", .width = 640, .height = 448});
    auto guestWindow =
        createGlfwWindow({.title = "Online guest test", .width = 640, .height = 448});
    glfwDefaultWindowHints();
    RenderDeviceDesc desc;
    desc.enableValidation = true;
    desc.shaderDirectory = paths::executableDirectory() / "shaders";
    auto hostRenderer = createVulkanRenderDevice(*hostWindow, desc);
    auto guestRenderer = createVulkanRenderDevice(*guestWindow, desc);
    if (threaded) {
        hostRenderer =
            std::make_unique<ThreadedRenderDevice>(std::move(hostRenderer), Extent2D{640, 448});
        guestRenderer =
            std::make_unique<ThreadedRenderDevice>(std::move(guestRenderer), Extent2D{640, 448});
    }
    exerciseLobby(*hostRenderer, *guestRenderer, [&] { hostWindow->pollEvents(); });
}
} // namespace
