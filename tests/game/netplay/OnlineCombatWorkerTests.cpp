#include <chrono>
#include <memory>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/OnlinePlayPair.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Pair = test::OnlinePlayPair;
constexpr usize kStage = static_cast<usize>(OnlineCombatReplay::Domain::Stage);

void finish(Pair& pair) {
    // A bounded scheduler barrier makes publication/lifetime assertions
    // deterministic. It is deliberately not a latency or throughput assertion.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto worker = pair.guest.combatReplayWorkerMetrics();
        if (!worker.inFlight && !worker.pending) {
            break;
        }
        std::this_thread::yield();
    }
    REQUIRE_FALSE(pair.guest.combatReplayWorkerMetrics().inFlight);
    REQUIRE_FALSE(pair.guest.combatReplayWorkerMetrics().pending);
    pair.draw(); // polls only copied completion; never the producer's owners
}

void admitted(Pair& pair, u64 previousAdoptions = 0) {
    const auto previousPresents = pair.guest.combatReplayWorkerMetrics().presents;
    for (s32 tick = 0; tick < 1200; ++tick) {
        SessionInputs::Frame accept;
        if (pair.host.hostScene()->scroll().active()) {
            for (auto& input : accept) {
                input.menu.select = true;
            }
        }
        pair.step(accept, accept);
        finish(pair);
        if (pair.guest.combatReplayWorkerMetrics().adoptions > previousAdoptions &&
            pair.guest.combatReplayWorkerMetrics().presents > previousPresents) {
            break;
        }
    }
    const auto worker = pair.guest.combatReplayWorkerMetrics();
    INFO("worker submissions=" << worker.submitted << " executed=" << worker.executed
                               << " publications=" << worker.publications
                               << " rejected=" << worker.rejectedPublications);
    REQUIRE(worker.stage);
    REQUIRE(worker.running);
    REQUIRE(worker.adoptions > previousAdoptions);
    REQUIRE(worker.presents > previousPresents);
    REQUIRE(worker.errors == 0);
    REQUIRE(worker.submissionRefusals == 0);
    REQUIRE(pair.guest.combatReplayDiagnostics().domains[kStage].passes > 0);
}

std::unique_ptr<Pair> running() {
    auto pair = std::make_unique<Pair>(0, false, false, 1);
    const auto level = pair->catalog.byName("G1");
    REQUIRE(level);
    pair->open(*level);
    admitted(*pair);
    return pair;
}

TEST_CASE("online Stage worker keeps input submission on main and joins before revocation",
          "[netplay][online-play][assets][online-combat-worker]") {
    auto owner = running();
    auto& pair = *owner;
    const auto before = pair.guest.combatReplayWorkerMetrics();
    const auto samples = pair.guest.predictionMetrics().samples;
    usize sends = 0;
    pair.wire.guestWire.beforeSend = [&](std::span<const u8> bytes, PacketTransport::Delivery) {
        if (InputPacket::decode(bytes)) {
            ++sends;
            CHECK(pair.guest.combatReplayWorkerMetrics().submitted == before.submitted);
            CHECK(pair.guest.predictionMetrics().samples == samples);
        }
        return PacketTransport::SendResult::Sent;
    };
    REQUIRE(pair.guest.update({}) != OnlinePlay::Phase::Failed);
    pair.wire.guestWire.beforeSend = {};
    REQUIRE(sends > 0);
    CHECK(pair.guest.predictionMetrics().samples > samples);
    finish(pair);

    REQUIRE(pair.guest.pause());
    CHECK(pair.guest.combatReplayWorkerMetrics().stops == before.stops);
    usize pausedInputs = 0;
    for (s32 tick = 0; tick < 4; ++tick) {
        pair.step();
        for (const auto& input : pair.wire.guest.sampledInputs()) {
            ++pausedInputs;
            CHECK(input.magnitude == 0);
            CHECK(input.heldButtons == 0);
            CHECK(input.pressedButtons == 0);
        }
        finish(pair);
    }
    CHECK(pausedInputs > 0);
    CHECK(pair.guest.phase() == OnlinePlay::Phase::Paused);
    CHECK(pair.guest.combatReplayWorkerMetrics().running);
    CHECK(pair.guest.combatReplayWorkerMetrics().stops == before.stops);
    REQUIRE(pair.guest.resume());

    const auto enabled = pair.guest.combatReplayWorkerMetrics();
    const auto advanced = pair.guest.combatReplayDiagnostics().domains[kStage].advancedTicks;
    pair.guest.setMovementPrediction(false);
    const auto disabled = pair.guest.combatReplayWorkerMetrics();
    CHECK_FALSE(disabled.running);
    CHECK_FALSE(disabled.inFlight);
    CHECK_FALSE(disabled.pending);
    CHECK(disabled.stops == enabled.stops + 1);
    pair.draw();
    CHECK(pair.guest.combatReplayWorkerMetrics().presents == enabled.presents);
    for (s32 tick = 0; tick < 2; ++tick) {
        pair.step();
        pair.draw();
    }
    CHECK(pair.guest.combatReplayWorkerMetrics().submitted == disabled.submitted);
    CHECK(pair.guest.combatReplayDiagnostics().domains[kStage].advancedTicks >= advanced);

    pair.guest.setMovementPrediction(true);
    admitted(pair, enabled.adoptions);
    const auto restarted = pair.guest.combatReplayWorkerMetrics();
    CHECK(restarted.starts == enabled.starts + 1);
    CHECK(restarted.executed >= enabled.executed);
    CHECK(restarted.submitted >= enabled.submitted);
    CHECK(pair.guest.combatReplayDiagnostics().domains[kStage].advancedTicks >= advanced);

    // Closing is also legal immediately after submission, without a test-side
    // scheduler barrier. The owner joins before clearing its borrowed assets.
    REQUIRE(pair.guest.update({}) != OnlinePlay::Phase::Failed);
    pair.guest.close();
    CHECK(pair.guest.phase() == OnlinePlay::Phase::Closed);
    CHECK(pair.guest.replica() == nullptr);
    CHECK_FALSE(pair.guest.combatReplayWorkerMetrics().running);
    CHECK_FALSE(pair.guest.combatReplayWorkerMetrics().inFlight);
    CHECK(pair.guest.combatReplayWorkerMetrics().stops == restarted.stops + 1);
    CHECK(pair.guest.combatReplayDiagnostics().domains[kStage].advancedTicks >= advanced);
}

TEST_CASE("online Stage worker authority expires on main updates not completed jobs",
          "[netplay][online-play][assets][online-combat-worker]") {
    auto owner = running();
    auto& pair = *owner;
    const auto before = pair.guest.combatReplayWorkerMetrics();
    // No host update or checkpoint publication while the guest continues to
    // sample. CPU scheduling must not extend the authority's 30-update bound.
    for (s32 tick = 0; tick < 35; ++tick) {
        REQUIRE(pair.guest.update({}) != OnlinePlay::Phase::Failed);
    }
    finish(pair);
    const auto expired = pair.guest.combatReplayWorkerMetrics();
    REQUIRE(expired.expired > before.expired);
    CHECK(expired.errors == 0);
    CHECK(expired.submissionRefusals == 0);
    for (s32 tick = 0; tick < 3; ++tick) {
        REQUIRE(pair.guest.update({}) != OnlinePlay::Phase::Failed);
        pair.draw();
    }
    CHECK(pair.guest.combatReplayWorkerMetrics().presents == expired.presents);
    CHECK(pair.guest.combatReplayWorkerMetrics().submitted == expired.submitted);
    CHECK(pair.guest.phase() == OnlinePlay::Phase::Playing);

    // A genuinely newer authenticated checkpoint reopens the same bounded lane;
    // no sent input is restamped and input collection never moved to the worker.
    admitted(pair, expired.adoptions);
    CHECK(pair.guest.combatReplayWorkerMetrics().starts == expired.starts);
    CHECK(pair.guest.combatReplayWorkerMetrics().errors == 0);
}
} // namespace
