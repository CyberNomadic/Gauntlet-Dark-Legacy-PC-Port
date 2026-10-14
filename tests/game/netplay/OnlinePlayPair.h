#pragma once

#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/OnlinePair.h"
#include "game/screens/OnlinePlay.h"

namespace gdl::test {
using namespace gdl::game;

struct OnlinePlayPair {
    std::filesystem::path root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice hostDevice;
    test::FakeRenderDevice guestDevice;
    GameConfig config;
    StringTable strings;
    LevelCatalog catalog;
    GameContext context;
    test::OnlinePair wire{{}, false, 2, 2};
    OnlineParty hostParty;
    OnlineParty guestParty;
    OnlinePlay host;
    OnlinePlay guest;

    explicit OnlinePlayPair(s32 classOffset = 0, bool pendingPromotion = false,
                            bool pendingRelic = false, s32 startingLevel = 60) {
        REQUIRE(catalog.load(root));
        context.config = &config;
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        context.strings = &strings;
        context.levels = &catalog;
        context.unpackedRoot = root;
        const auto choice = [pendingPromotion, pendingRelic,
                             startingLevel](s32 device, s32 character, s32 color) {
            CharacterSave save;
            save.name = "ONLINE";
            save.character = character;
            save.color = color;
            save.progress().experience = levelExperience(startingLevel);
            if (startingLevel == 1) {
                save.autoAttack = false;
            }
            save.progress().promotedLevel = pendingPromotion ? 20 : -1;
            if (pendingRelic) {
                save.progress().relics.runes = save.progress().relics.pendingRunes = 1;
            }
            save.progress().health = 9000;
            save.classUnlock = 0xFFFF;
            save.moviesSeen = {"PRIVATE"};
            save.classes[static_cast<usize>((character + 1) % kClassCount)].experience = 123456;
            return PartyMember{device, save, static_cast<usize>(10 + device)};
        };
        REQUIRE(hostParty.select(wire.hostSession, std::array{choice(1, classOffset, 0),
                                                              choice(3, classOffset + 1, 1)}));
        REQUIRE(guestParty.select(wire.guestSession, std::array{choice(0, classOffset + 2, 2),
                                                                choice(2, classOffset + 3, 3)}));
        wire.start();
    }
    void open(const LevelRef& level = LevelRef::tower(), PlayOptions options = {}) {
        options.welcome = false;
        if (level.isTower() && !options.position) {
            options.position = Vec3{0, 0, 0}; // bypass the tower entry pan for immediate input
        }
        REQUIRE(guest.open(guestDevice, context, wire.guestSession, guestParty));
        REQUIRE(host.open(hostDevice, context, wire.hostSession, hostParty, level, options));
        // The host cannot simulate before the guest has loaded its own resources.
        CHECK(host.update({}) == OnlinePlay::Phase::Loading);
        CHECK(wire.host.tick() == 0);
        CHECK_FALSE(host.shown());
        for (s32 i = 0; i < 3600 && host.phase() != OnlinePlay::Phase::Playing; ++i) {
            step();
        }
        REQUIRE(host.phase() == OnlinePlay::Phase::Playing);
        REQUIRE(guest.phase() == OnlinePlay::Phase::Playing);
    }
    void step(const SessionInputs::Frame& hostInputs = {},
              const SessionInputs::Frame& guestInputs = {}) {
        REQUIRE(host.update(hostInputs) != OnlinePlay::Phase::Failed);
        REQUIRE(guest.update(guestInputs) != OnlinePlay::Phase::Failed);
        if (host.entryVisible()) {
            hostDevice.draws.clear();
            host.render(hostDevice, Mat4{1}, 1280, 720, 1.0 / 60, 1);
        }
        if (guest.entryVisible()) {
            draw();
        }
    }
    void draw() {
        guestDevice.draws.clear();
        guest.render(guestDevice, Mat4{1}, 1280, 720, 1.0 / 60, 1);
        REQUIRE(guest.phase() != OnlinePlay::Phase::Failed);
    }
};

} // namespace gdl::test
