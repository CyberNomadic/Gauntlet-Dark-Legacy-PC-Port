#include <string_view>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include "LatencyProbeEligibility.h"
#include "game/netplay/LatencyTelemetry.h"
#include "game/players/PlayerAnimator.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using namespace std::chrono_literals;

InputCommand throwInput(const CombatSnapshot& state, u64 tick = 0) {
    InputCommand input;
    input.epoch = state.motion.epoch;
    input.tick = tick;
    input.seat = 1;
    input.grant = state.motion.players[1]->grant;
    input.heldButtons = static_cast<u32>(CommandHeld::Attack);
    input.pressedButtons = static_cast<u32>(CommandPress::Attack);
    return input;
}
InputCommand consumedInput(const CombatSnapshot& state, u64 tick) {
    auto input = throwInput(state, tick);
    input.heldButtons = 0;
    input.pressedButtons = 0;
    return input;
}
CombatSnapshot projectileState() {
    CombatSnapshot state;
    state.motion.epoch = 1;
    state.motion.tick = 10;
    state.motion.players[1] = SeatMotion{4, 1, Vec3{0}, 0};
    state.players[1] = PlayerCombatState{};
    state.motion.players[2] = SeatMotion{7, 1, Vec3{0}, 0};
    state.players[2] = PlayerCombatState{};
    return state;
}
ProjectileState projectile(const ShotIdentity& origin) {
    ProjectileState result;
    result.instance = 7;
    result.resource = 1;
    result.rollback = ProjectileState::Rollback{};
    result.rollback->origin = origin;
    REQUIRE(result.valid());
    return result;
}
CombatSnapshot motionAuthority() {
    auto result = projectileState();
    result.players[1]->movement.enabled = true;
    result.players[1]->movement.current = true;
    result.players[1]->animation.generation = 1;
    return result;
}
InputCommand movementInput(const CombatSnapshot& authority) {
    auto result = consumedInput(authority, authority.motion.tick + 1);
    result.direction = {1, 0};
    result.magnitude = 1;
    return result;
}

TEST_CASE("latency uses monotonic wall time and separates queue resolution and presentation",
          "[netplay][latency]") {
    LatencyTelemetry trace;
    trace.begin(1);
    InputCommand input;
    input.epoch = 1;
    input.grant = 1;
    input.tick = 10;
    const std::array commands{input};
    const LatencyTelemetry::Time start{};
    trace.sampled(commands, start);
    trace.sampled(commands, start + 5ms); // redundant commands retain the original sample time
    trace.sent(commands, start + 17ms);
    trace.sent(commands, start + 30ms);
    CombatSnapshot state;
    state.motion.epoch = 1;
    state.motion.tick = 9;
    trace.checkpoint(state, start + 70ms);
    CHECK(trace.resolved().summary().count == 0);
    state.motion.tick = 10;
    trace.checkpoint(state, start + 100ms);
    trace.checkpoint(state, start + 120ms);
    trace.presented(state, start + 150ms);
    const auto report = nlohmann::json::parse(trace.report());
    CHECK(report["sample_to_send"]["count"] == 1);
    CHECK(report["sample_to_send"]["p95_ms"] == 17);
    CHECK(report["sample_to_resolved"]["p95_ms"] == 100);
    CHECK(report["sample_to_confirmed_frame"]["p95_ms"] == 150);
    CHECK(report["pending"] == 0);
    CHECK_FALSE(report.contains("relayed"));
    trace.network({-1, 0, 0, false});
    CHECK(nlohmann::json::parse(trace.report())["rtt"]["count"] == 0);
    trace.network({15, 0, 0, true});
    trace.network({5, 0, 0, false});
    CHECK(nlohmann::json::parse(trace.report())["path_changes"] == 1);
    trace.begin(2);
    CHECK(trace.resolved().summary().count == 0);
}

TEST_CASE("motion latency requires a visible change and counts blocked or stalled starts",
          "[netplay][latency]") {
    LatencyTelemetry trace;
    trace.begin(1);
    CombatSnapshot state;
    state.motion.epoch = 1;
    state.motion.players[1] = SeatMotion{1, 1, Vec3{0}, 0};
    state.players[1] = PlayerCombatState{};
    state.players[1]->movement.enabled = true;
    state.motion.tick = 10;
    const auto authority = state;
    state.players[1]->movement = {}; // Native Stage drawing results contain no replay certificate.
    InputCommand input;
    input.epoch = 1;
    input.grant = 1;
    input.seat = 1;
    input.tick = 11;
    input.direction = {1, 0};
    input.magnitude = 1;
    const LatencyTelemetry::Time start{};
    trace.checkpoint(authority, start);
    trace.motionRequest(std::array{input}, &state, &authority, start);
    trace.presented(state, start + 16ms);
    CHECK(trace.firstMotion().summary().count == 0);
    state.motion.players[1]->position.x = 0.1f;
    trace.presented(state, start + 33ms);
    CHECK(trace.firstMotion().summary().p95 == 33);
    input.magnitude = 0;
    trace.motionRequest(std::array{input}, &state, &authority, start + 34ms);
    input.magnitude = 1;
    trace.motionRequest(std::array{input}, &state, &authority, start + 35ms);
    trace.update(start + 2s);
    CHECK(trace.missedMotion() == 1);
    CHECK(trace.firstMotion().summary().count == 1);
}

TEST_CASE("motion latency uses current authority with delayed or predicted drawing origins",
          "[netplay][latency][probe-eligibility]") {
    const auto shownTick = GENERATE(u64{8}, u64{12});
    const auto authority = motionAuthority();
    auto shown = authority;
    shown.motion.tick = shownTick;
    shown.players[1]->movement = {};
    shown.players[1]->rollback.reset();
    const auto input = movementInput(authority);
    LatencyTelemetry trace;
    trace.begin(authority.motion.epoch);
    const LatencyTelemetry::Time start{};
    trace.checkpoint(authority, start);
    trace.motionRequest(std::array{input}, &shown, &authority, start + 1ms);
    trace.motionRequest(std::array{input}, &shown, &authority, start + 5ms);
    trace.presented(shown, start + 9ms);
    CHECK(trace.firstMotion().summary().count == 0);
    shown.motion.players[1]->position.x += 0.1f;
    trace.presented(shown, start + 17ms);
    trace.presented(shown, start + 20ms);
    CHECK(trace.firstMotion().summary().count == 1);
    CHECK(trace.firstMotion().summary().p95 == 16);
    CHECK(trace.missedMotion() == 0);
}

TEST_CASE("motion latency counts refused movement edges without certifying unrelated authority",
          "[netplay][latency][probe-eligibility]") {
    const auto refusal = GENERATE("unobserved", "absent", "historical", "future", "epoch", "grant",
                                  "continuity", "position", "missing-player", "missing-motion",
                                  "disabled", "dying", "portal", "past-input", "wrong-input-grant");
    CAPTURE(refusal);
    const LatencyTelemetry::Time start{};
    auto authority = motionAuthority();
    auto shown = authority;
    shown.players[1]->movement = {};
    auto input = movementInput(authority);
    LatencyTelemetry trace;
    trace.begin(authority.motion.epoch);
    if (std::string_view{refusal} != "unobserved") {
        trace.checkpoint(authority, start);
    }
    const CombatSnapshot* source = &authority;
    if (std::string_view{refusal} == "absent") {
        source = nullptr;
    } else if (std::string_view{refusal} == "historical") {
        --authority.motion.tick;
        trace.checkpoint(authority, start + 1ms); // Reordering cannot lower the trusted fence.
    } else if (std::string_view{refusal} == "future") {
        ++authority.motion.tick;
    } else if (std::string_view{refusal} == "epoch") {
        ++authority.motion.epoch;
    } else if (std::string_view{refusal} == "grant") {
        ++authority.motion.players[1]->grant;
    } else if (std::string_view{refusal} == "continuity") {
        ++authority.motion.players[1]->continuity;
    } else if (std::string_view{refusal} == "position") {
        authority.motion.players[1]->position.x += 1;
    } else if (std::string_view{refusal} == "missing-player") {
        authority.players[1].reset();
    } else if (std::string_view{refusal} == "missing-motion") {
        authority.motion.players[1].reset();
    } else if (std::string_view{refusal} == "disabled") {
        authority.players[1]->movement.enabled = false;
    } else if (std::string_view{refusal} == "dying") {
        authority.players[1]->life = ReplicaPlayerLife::Dying;
    } else if (std::string_view{refusal} == "portal") {
        authority.players[1]->portalPhase = 0.0f;
    } else if (std::string_view{refusal} == "past-input") {
        input.tick = authority.motion.tick;
    } else if (std::string_view{refusal} == "wrong-input-grant") {
        ++input.grant;
    }
    trace.motionRequest(std::array{input}, &shown, source, start + 2ms);
    trace.motionRequest(std::array{input}, &shown, source, start + 3ms);
    CHECK(trace.missedMotion() == 1); // A held rejected edge is not a stream of new requests.
    shown.motion.players[1]->position.x += 0.1f;
    trace.presented(shown, start + 17ms);
    trace.update(start + 2s);
    CHECK(trace.firstMotion().summary().count == 0);
    CHECK(trace.missedMotion() == 1);
}

TEST_CASE("motion latency rejects mismatched visual owners and interrupted post-draw observations",
          "[netplay][latency][probe-eligibility]") {
    const bool beforeRequest = GENERATE(false, true);
    const auto refusal = GENERATE("epoch", "grant", "continuity", "missing-player",
                                  "missing-motion", "dying", "portal");
    CAPTURE(beforeRequest, refusal);
    const LatencyTelemetry::Time start{};
    const auto authority = motionAuthority();
    auto shown = authority;
    shown.players[1]->movement = {};
    const auto input = movementInput(authority);
    LatencyTelemetry trace;
    trace.begin(authority.motion.epoch);
    trace.checkpoint(authority, start);
    if (!beforeRequest) {
        trace.motionRequest(std::array{input}, &shown, &authority, start + 1ms);
    }
    shown.motion.players[1]->position.x += 0.1f;
    if (std::string_view{refusal} == "epoch") {
        ++shown.motion.epoch;
    } else if (std::string_view{refusal} == "grant") {
        ++shown.motion.players[1]->grant;
    } else if (std::string_view{refusal} == "continuity") {
        ++shown.motion.players[1]->continuity;
    } else if (std::string_view{refusal} == "missing-player") {
        shown.players[1].reset();
    } else if (std::string_view{refusal} == "missing-motion") {
        shown.motion.players[1].reset();
    } else if (std::string_view{refusal} == "dying") {
        shown.players[1]->life = ReplicaPlayerLife::Dying;
    } else if (std::string_view{refusal} == "portal") {
        shown.players[1]->portalPhase = 0.0f;
    }
    if (beforeRequest) {
        trace.motionRequest(std::array{input}, &shown, &authority, start + 1ms);
        CHECK(trace.missedMotion() == 1);
    }
    trace.presented(shown, start + 17ms);
    trace.update(start + 2s);
    CHECK(trace.firstMotion().summary().count == 0);
    CHECK(trace.missedMotion() == 1);
}

TEST_CASE("motion latency ignores a historical draw before observing the actual request origin",
          "[netplay][latency][probe-eligibility]") {
    const auto authority = motionAuthority();
    auto shown = authority;
    shown.players[1]->movement = {};
    const auto input = movementInput(authority);
    LatencyTelemetry trace;
    trace.begin(authority.motion.epoch);
    const LatencyTelemetry::Time start{};
    trace.checkpoint(authority, start);
    trace.motionRequest(std::array{input}, &shown, &authority, start);
    --shown.motion.tick;
    shown.motion.players[1]->position.x += 0.1f;
    trace.presented(shown, start + 10ms);
    CHECK(trace.firstMotion().summary().count == 0);
    ++shown.motion.tick;
    trace.presented(shown, start + 25ms);
    CHECK(trace.firstMotion().summary().p95 == 25);
    CHECK(trace.missedMotion() == 0);
}

TEST_CASE("probe throw readiness separates visual idle from native authority permission",
          "[netplay][latency][probe-eligibility]") {
    using Eligibility = test::LatencyProbeEligibility;
    auto authority = motionAuthority();
    authority.players[1]->rollback = PlayerAnimator::Checkpoint{};
    authority.players[1]->rollback->action = PlayerAnimator::Action::Ready;
    auto shown = authority;
    shown.players[1]->movement = {};
    shown.players[1]->rollback.reset();
    const u64 epoch = authority.motion.epoch;
    const u32 grant = authority.motion.players[1]->grant;
    CHECK(Eligibility::ready(&shown, &authority, epoch, 1, grant));
    CHECK(Eligibility::idleForThrow(&shown, &authority, epoch, 1, grant));
    CHECK_FALSE(Eligibility::authorityIdle(&shown, 1));
    CHECK_FALSE(Eligibility::ready(&shown, &authority, epoch + 1, 1, grant));
    CHECK_FALSE(Eligibility::ready(&shown, &authority, epoch, 1, grant + 1));
    CHECK_FALSE(Eligibility::ready(nullptr, &authority, epoch, 1, grant));
    CHECK_FALSE(Eligibility::ready(&shown, nullptr, epoch, 1, grant));
    CHECK_FALSE(Eligibility::ready(&shown, &authority, epoch, InputCommand::kSeats, grant));

    SECTION("host native action overrides a stale idle presentation cursor") {
        authority.players[1]->rollback->action = PlayerAnimator::Action::Throw;
    }
    SECTION("displayed throw blocks a new sparse request despite idle authority") {
        shown.players[1]->animation.action = static_cast<u32>(PlayerAnimator::Action::Throw);
    }
    SECTION("host movement permission is still required") {
        authority.players[1]->movement.enabled = false;
    }
    SECTION("the displayed origin cannot belong to a previous occupant") {
        ++shown.motion.players[1]->grant;
    }
    SECTION("the displayed origin cannot precede a host teleport") {
        ++authority.motion.players[1]->continuity;
    }
    SECTION("departure is not an input-ready standing body") {
        shown.players[1]->portalPhase = 0.0f;
    }
    SECTION("a host dead body is not an input-ready standing body") {
        authority.players[1]->life = ReplicaPlayerLife::Dying;
    }
    CHECK_FALSE(Eligibility::idleForThrow(&shown, &authority, epoch, 1, grant));
}

TEST_CASE("throw latency requires a new rendered throw and counts missing attacks",
          "[netplay][latency]") {
    LatencyTelemetry trace;
    trace.begin(1);
    CombatSnapshot state;
    state.motion.epoch = 1;
    state.motion.players[1] = SeatMotion{1, 1, Vec3{0}, 0};
    state.players[1] = PlayerCombatState{};
    auto& animation = state.players[1]->animation;
    animation.generation = 1;
    const LatencyTelemetry::Time start{};
    trace.throwRequest(throwInput(state), state, start);
    // Neither moving nor receiving a resolved checkpoint proves an attack rendered.
    state.motion.players[1]->position.x = 0.2f;
    state.motion.tick = 10;
    trace.checkpoint(state, start + 10ms);
    trace.presented(state, start + 15ms);
    animation.action = static_cast<u8>(PlayerAnimator::Action::Throw);
    trace.presented(state, start + 20ms); // same generation is an old animation
    CHECK(nlohmann::json::parse(trace.report())["throw_to_first_frame"]["count"] == 0);
    ++animation.generation;
    animation.action = static_cast<u8>(PlayerAnimator::Action::HitReact);
    trace.presented(state, start + 30ms);
    CHECK(nlohmann::json::parse(trace.report())["throw_to_first_frame"]["count"] == 0);
    ++animation.generation;
    animation.action = static_cast<u8>(PlayerAnimator::Action::ThrowRelease);
    trace.presented(state, start + 80ms);
    trace.presented(state, start + 100ms);
    CHECK(nlohmann::json::parse(trace.report())["throw_to_first_frame"]["count"] == 1);
    CHECK(nlohmann::json::parse(trace.report())["throw_to_first_frame"]["p95_ms"] == 80);
    animation.action = static_cast<u32>(PlayerAnimator::Action::Ready);
    trace.throwRequest(throwInput(state), state, start + 100ms);
    trace.update(start + 2s);
    CHECK(nlohmann::json::parse(trace.report())["missed_throws"] == 1);
    trace.throwRequest(throwInput(state), state, start + 2s);
    ++state.motion.players[1]->continuity;
    trace.presented(state, start + 2100ms);
    CHECK(nlohmann::json::parse(trace.report())["missed_throws"] == 2);
    trace.throwRequest(throwInput(state), state, start + 2200ms);
    trace.begin(2);
    trace.update(start + 4s);
    CHECK(nlohmann::json::parse(trace.report())["missed_throws"] == 0);
}

TEST_CASE("latency distributions are bounded and do not turn missing data into zero",
          "[netplay][latency]") {
    LatencyTelemetry::Distribution samples;
    CHECK(samples.summary().count == 0);
    samples.add(-1);
    for (s32 i = 0; i < 1000; ++i) {
        samples.add(i);
    }
    CHECK(samples.summary().count == 1000);
    CHECK(samples.summary().window == 512);
    CHECK(samples.summary().maximum == 999);
    CHECK(samples.summary().p95 == 974);
}

TEST_CASE("predicted throws retain independent authoritative action timing",
          "[netplay][latency][rollback]") {
    using Action = PlayerAnimator::Action;
    LatencyTelemetry trace;
    trace.begin(1);
    CombatSnapshot host;
    host.motion.epoch = 1;
    host.motion.tick = 10;
    host.motion.players[1] = SeatMotion{1, 1, Vec3{0}, 0};
    host.players[1] = PlayerCombatState{};
    host.players[1]->animation.generation = 1;
    const LatencyTelemetry::Time start{};
    trace.checkpoint(host, start);
    trace.throwRequest(throwInput(host, 11), host, start + 1ms);
    auto predicted = host;
    predicted.players[1]->animation.action = static_cast<u32>(Action::Throw);
    predicted.players[1]->animation.generation = 2;
    trace.presented(predicted, start + 17ms);
    auto report = nlohmann::json::parse(trace.report());
    CHECK(report["throw_to_first_frame"]["p95_ms"] == 16);
    CHECK(report["throw_to_authority"]["count"] == 0);
    CHECK(report["pending_throw_authority"] == 1);

    // A correction back to idle does not erase the unresolved host observation.
    ++host.motion.tick;
    trace.checkpoint(host, start + 40ms);
    trace.presented(host, start + 50ms);
    CHECK(nlohmann::json::parse(trace.report())["pending_throw_authority"] == 1);
    ++host.motion.tick;
    ++host.players[1]->animation.generation;
    host.players[1]->animation.action = static_cast<u32>(Action::HitReact);
    trace.checkpoint(host, start + 70ms);
    CHECK(nlohmann::json::parse(trace.report())["throw_to_authority"]["count"] == 0);

    ++host.motion.tick;
    ++host.players[1]->animation.generation;
    host.players[1]->animation.action = static_cast<u32>(Action::ThrowRelease);
    host.players[1]->rollbackInput = consumedInput(host, host.motion.tick);
    trace.checkpoint(host, start + 121ms);
    trace.checkpoint(host, start + 140ms);
    trace.presented(host, start + 150ms);
    report = nlohmann::json::parse(trace.report());
    CHECK(report["throw_to_first_frame"]["count"] == 1);
    CHECK(report["throw_to_first_frame"]["p95_ms"] == 16);
    CHECK(report["throw_to_authority"]["count"] == 1);
    CHECK(report["throw_to_authority"]["p95_ms"] == 120);
    CHECK(report["pending_throw_authority"] == 0);
    CHECK(report["expired_throw_authority"] == 0);
    CHECK(report["interrupted_throw_authority"] == 0);
    CHECK(report["superseded_throw_authority"] == 0);
}

TEST_CASE("throw confirmation uses the native host generation not a predicted display cursor",
          "[netplay][latency][rollback]") {
    using Action = PlayerAnimator::Action;
    LatencyTelemetry trace;
    trace.begin(1);
    CombatSnapshot host;
    host.motion.epoch = 1;
    host.motion.tick = 10;
    host.motion.players[1] = SeatMotion{1, 1, Vec3{0}, 0};
    host.players[1] = PlayerCombatState{};
    host.players[1]->rollback = PlayerAnimator::Checkpoint{};
    host.players[1]->rollback->animation.generation = 7;
    const LatencyTelemetry::Time start{};
    trace.checkpoint(host, start);
    auto shown = host;
    shown.players[1]->animation.generation = 1000;
    trace.throwRequest(throwInput(shown, 11), shown, start + 1ms);
    ++host.motion.tick;
    host.players[1]->animation.generation = 2000;
    host.players[1]->animation.action = static_cast<u32>(Action::Throw);
    host.players[1]->rollback->action = Action::Throw;
    trace.checkpoint(host, start + 10ms); // native generation has not changed
    CHECK(nlohmann::json::parse(trace.report())["throw_to_authority"]["count"] == 0);
    auto stale = host;
    --stale.motion.tick;
    stale.players[1]->rollback->animation.generation = 6;
    trace.checkpoint(stale, start + 20ms);
    CHECK(nlohmann::json::parse(trace.report())["throw_to_authority"]["count"] == 0);
    ++host.motion.tick;
    ++host.players[1]->rollback->animation.generation;
    host.players[1]->rollbackInput = consumedInput(host, host.motion.tick);
    trace.checkpoint(host, start + 31ms);
    auto report = nlohmann::json::parse(trace.report());
    CHECK(report["throw_to_authority"]["p95_ms"] == 30);
    CHECK(report["throw_to_first_frame"]["count"] == 0);
    trace.update(start + 2s);
    report = nlohmann::json::parse(trace.report());
    CHECK(report["missed_throws"] == 1);
    CHECK(report["expired_throw_authority"] == 0);
}

TEST_CASE("unconfirmed predicted throws expire independently and retain failure reasons",
          "[netplay][latency][rollback]") {
    using Action = PlayerAnimator::Action;
    LatencyTelemetry trace;
    trace.begin(1);
    CombatSnapshot state;
    state.motion.epoch = 1;
    state.motion.players[1] = SeatMotion{1, 1, Vec3{0}, 0};
    state.players[1] = PlayerCombatState{};
    state.players[1]->animation.generation = 1;
    const LatencyTelemetry::Time start{};
    trace.throwRequest(throwInput(state), state, start);
    auto predicted = state;
    ++predicted.players[1]->animation.generation;
    predicted.players[1]->animation.action = static_cast<u32>(Action::Throw);
    trace.presented(predicted, start + 16ms);
    trace.checkpoint(state, start + 120ms);
    trace.update(start + 2s);
    auto report = nlohmann::json::parse(trace.report());
    CHECK(report["throw_to_first_frame"]["count"] == 1);
    CHECK(report["throw_to_authority"]["count"] == 0);
    CHECK(report["expired_throw_authority"] == 1);
    CHECK(report["missed_throws"] == 0);

    trace.throwRequest(throwInput(state), state, start + 2100ms);
    trace.throwRequest(throwInput(state), state, start + 2200ms);
    ++state.motion.tick;
    ++state.motion.players[1]->continuity;
    trace.checkpoint(state, start + 2300ms);
    report = nlohmann::json::parse(trace.report());
    CHECK(report["superseded_throw_authority"] == 1);
    CHECK(report["interrupted_throw_authority"] == 1);
    CHECK(report["pending_throw_authority"] == 0);

    trace.throwRequest(throwInput(state), state, start + 2400ms);
    ++state.motion.tick;
    ++state.motion.players[1]->grant;
    trace.checkpoint(state, start + 2500ms);
    CHECK(nlohmann::json::parse(trace.report())["interrupted_throw_authority"] == 2);
    trace.throwRequest(throwInput(state), state, start + 2600ms);
    ++state.motion.tick;
    state.players[1].reset();
    trace.checkpoint(state, start + 2700ms);
    CHECK(nlohmann::json::parse(trace.report())["interrupted_throw_authority"] == 3);
    state.players[1] = PlayerCombatState{};
    trace.throwRequest(throwInput(state), state, start + 2800ms);
    trace.begin(2);
    trace.update(start + 4s);
    report = nlohmann::json::parse(trace.report());
    CHECK(report["throw_to_first_frame"]["count"] == 0);
    CHECK(report["throw_to_authority"]["count"] == 0);
    CHECK(report["pending_throw_authority"] == 0);
    CHECK(report["expired_throw_authority"] == 0);
    CHECK(report["interrupted_throw_authority"] == 0);
    CHECK(report["superseded_throw_authority"] == 0);
}

TEST_CASE("an earlier throw cannot confirm a later sampled command even after its deadline",
          "[netplay][latency][rollback]") {
    using Action = PlayerAnimator::Action;
    LatencyTelemetry trace;
    trace.begin(1);
    CombatSnapshot host;
    host.motion.epoch = 1;
    host.motion.tick = 100;
    host.motion.players[1] = SeatMotion{4, 1, Vec3{0}, 0};
    host.players[1] = PlayerCombatState{};
    host.players[1]->rollback = PlayerAnimator::Checkpoint{};
    host.players[1]->rollback->animation.generation = 7;
    const LatencyTelemetry::Time start{};
    trace.checkpoint(host, start);
    const auto command = throwInput(host, 110);
    trace.throwRequest(command, host, start + 1ms);
    host.motion.tick = 105;
    host.players[1]->rollback->action = Action::ThrowRelease;
    host.players[1]->rollback->animation.generation = 9;
    trace.checkpoint(host, start + 10ms);
    auto report = nlohmann::json::parse(trace.report());
    CHECK(report["throw_to_authority"]["count"] == 0);
    CHECK(report["pending_throw_authority"] == 0);
    CHECK(report["interrupted_throw_authority"] == 1);
    host.motion.tick = 110;
    ++host.players[1]->rollback->animation.generation;
    host.players[1]->rollbackInput = consumedInput(host, 110);
    trace.checkpoint(host, start + 31ms);
    report = nlohmann::json::parse(trace.report());
    CHECK(report["throw_to_authority"]["count"] == 0);
    CHECK(report["pending_throw_authority"] == 0);
}

TEST_CASE("throw observation remains pending without consumed input at the sampled deadline",
          "[netplay][latency][rollback]") {
    using Action = PlayerAnimator::Action;
    LatencyTelemetry trace;
    trace.begin(1);
    CombatSnapshot host;
    host.motion.epoch = 1;
    host.motion.tick = 100;
    host.motion.players[1] = SeatMotion{4, 1, Vec3{0}, 0};
    host.players[1] = PlayerCombatState{};
    const LatencyTelemetry::Time start{};
    trace.checkpoint(host, start);
    trace.throwRequest(throwInput(host, 110), host, start + 1ms);
    host.motion.tick = 110;
    host.players[1]->animation.action = static_cast<u32>(Action::ThrowRelease);
    host.players[1]->animation.generation = 9;
    trace.checkpoint(host, start + 10ms);
    CHECK(nlohmann::json::parse(trace.report())["pending_throw_authority"] == 1);
    host.motion.tick = 111;
    host.players[1]->rollbackInput = consumedInput(host, 109);
    trace.checkpoint(host, start + 20ms);
    CHECK(nlohmann::json::parse(trace.report())["pending_throw_authority"] == 1);
    host.motion.tick = 112;
    host.players[1]->rollbackInput = consumedInput(host, 113); // impossible future execution
    trace.checkpoint(host, start + 25ms);
    CHECK(nlohmann::json::parse(trace.report())["throw_to_authority"]["count"] == 0);
    host.motion.tick = 113;
    host.players[1]->rollbackInput = consumedInput(host, 112); // later neutral is valid evidence
    trace.checkpoint(host, start + 31ms);
    const auto report = nlohmann::json::parse(trace.report());
    CHECK(report["throw_to_authority"]["p95_ms"] == 30);
    CHECK(report["pending_throw_authority"] == 0);
}

TEST_CASE("throw probes start only with idle displayed and authoritative action baselines",
          "[netplay][latency][rollback]") {
    using Action = PlayerAnimator::Action;
    const LatencyTelemetry::Time start{};
    for (const bool hostBusy : {false, true}) {
        LatencyTelemetry trace;
        trace.begin(1);
        CombatSnapshot host;
        host.motion.epoch = 1;
        host.motion.tick = 100;
        host.motion.players[1] = SeatMotion{4, 1, Vec3{0}, 0};
        host.players[1] = PlayerCombatState{};
        auto shown = host;
        (hostBusy ? host : shown).players[1]->animation.action = static_cast<u32>(Action::Throw);
        trace.checkpoint(host, start);
        trace.throwRequest(throwInput(shown, 110), shown, start + 1ms);
        const auto report = nlohmann::json::parse(trace.report());
        CHECK(report["pending_throw_authority"] == 0);
        trace.update(start + 2s);
        CHECK(nlohmann::json::parse(trace.report())["missed_throws"] == 0);
    }
}

TEST_CASE("throw probes reject foreign command identity without replacing pending observations",
          "[netplay][latency][rollback]") {
    LatencyTelemetry trace;
    trace.begin(1);
    CombatSnapshot shown;
    shown.motion.epoch = 1;
    shown.motion.tick = 10;
    shown.motion.players[1] = SeatMotion{4, 1, Vec3{0}, 0};
    shown.players[1] = PlayerCombatState{};
    const LatencyTelemetry::Time start{};
    const auto command = throwInput(shown, 20);
    trace.throwRequest(command, shown, start);
    for (usize field = 0; field < 5; ++field) {
        auto invalid = command;
        switch (field) {
        case 0: ++invalid.epoch; break;
        case 1: ++invalid.grant; break;
        case 2: invalid.seat = 0; break;
        case 3: invalid.pressedButtons = 0; break;
        case 4: invalid.seat = 4; break;
        default: FAIL("Unknown invalid command field"); break;
        }
        trace.throwRequest(invalid, shown, start + 10ms);
    }
    const auto report = nlohmann::json::parse(trace.report());
    CHECK(report["pending_throw_authority"] == 1);
    CHECK(report["missed_throws"] == 0);
    CHECK(report["superseded_throw_authority"] == 0);
    trace.update(start + 1001ms);
    CHECK(nlohmann::json::parse(trace.report())["expired_throw_authority"] == 1);
}

TEST_CASE("native projectile telemetry separates release display and exact host observation",
          "[netplay][latency][rollback]") {
    LatencyTelemetry trace;
    trace.begin(1);
    auto state = projectileState();
    const LatencyTelemetry::Time start{};
    trace.checkpoint(state, start);
    trace.throwRequest(throwInput(state, 11), state, start + 1ms);
    const ShotIdentity shot{1, 14, 3, 4, 1, false};
    trace.localRelease(shot, start + 31ms);
    auto shown = state;
    shown.players[1]->animation.action = static_cast<u32>(PlayerAnimator::Action::ThrowRelease);
    ++shown.players[1]->animation.generation;
    trace.presented(shown, start + 35ms);
    auto report = nlohmann::json::parse(trace.report());
    CHECK(report["schema"] == 3);
    CHECK(report["throw_to_first_frame"]["count"] == 1);
    CHECK(report["release_to_first_projectile_frame"]["count"] == 0);
    CHECK(report["release_to_host_projectile"]["count"] == 0);
    shown.projectiles.push_back(projectile(shot));
    trace.presented(shown, start + 39ms);   // mixed-age local release leads the world's tick
    trace.localRelease(shot, start + 45ms); // replay is not a new release timestamp
    trace.presented(shown, start + 55ms);
    report = nlohmann::json::parse(trace.report());
    CHECK(report["release_to_first_projectile_frame"]["count"] == 1);
    CHECK(report["release_to_first_projectile_frame"]["p95_ms"] == 8);
    CHECK(report["release_to_host_projectile"]["count"] == 0);
    state.motion.tick = shot.tick;
    state.projectiles.push_back(projectile(shot));
    trace.checkpoint(state, start + 131ms);
    trace.checkpoint(state, start + 150ms);
    trace.localRelease(shot, start + 160ms);
    trace.presented(state, start + 170ms);
    report = nlohmann::json::parse(trace.report());
    CHECK(report["release_to_first_projectile_frame"]["count"] == 1);
    CHECK(report["release_to_host_projectile"]["count"] == 1);
    CHECK(report["release_to_host_projectile"]["p95_ms"] == 100);
    CHECK(report["pending_releases"] == 0);
}

TEST_CASE("turbo telemetry measures each same-tick emission without replay duplicates",
          "[netplay][latency][rollback][turbo-volley-identity]") {
    LatencyTelemetry trace;
    trace.begin(1);
    auto host = projectileState();
    const LatencyTelemetry::Time start{};
    trace.checkpoint(host, start);
    const ShotIdentity first{1, 14, 3, 4, 1, false, 0, 0x00010000};
    auto second = first;
    second.emission = 0x00010001;
    auto third = first;
    third.emission = 0x00020000;
    for (const auto& origin : {first, second, third}) {
        trace.localRelease(origin, start + 10ms);
        trace.localRelease(origin, start + 11ms);
    }
    auto shown = host;
    shown.projectiles = {projectile(first), projectile(third)};
    shown.projectiles.back().instance += 1;
    trace.presented(shown, start + 20ms);
    host.motion.tick = 14;
    host.projectiles = shown.projectiles;
    trace.checkpoint(host, start + 60ms);
    auto report = nlohmann::json::parse(trace.report(true));
    CHECK(report["release_sample_count"] == 3);
    CHECK(report["release_to_first_projectile_frame"]["count"] == 2);
    CHECK(report["release_to_host_projectile"]["count"] == 2);
    CHECK(report["unobserved_host_projectiles"] == 1);
    CHECK(report["pending_releases"] == 0);
    REQUIRE(report["release_samples"].size() == 3);
    CHECK(report["release_samples"][0]["emission"] == first.emission);
    CHECK(report["release_samples"][1]["emission"] == second.emission);
    CHECK(report["release_samples"][2]["emission"] == third.emission);
    CHECK(report["release_samples"][0]["fan_ordinal"] == 0);
    CHECK(report["release_samples"][1]["outcome"] == "unobserved");
    trace.update(start + 2s);
    for (const auto& origin : {first, second, third}) {
        trace.localRelease(origin, start + 3s);
    }
    CHECK(nlohmann::json::parse(trace.report(true))["release_sample_count"] == 3);
}

TEST_CASE("turbo same-tick telemetry is bounded and retains the ordinary rewind fence",
          "[netplay][latency][rollback][turbo-volley-identity]") {
    LatencyTelemetry trace;
    trace.begin(1);
    const auto host = projectileState();
    const LatencyTelemetry::Time start{};
    trace.checkpoint(host, start);
    ShotIdentity origin{1, 14, 3, 4, 1, false};
    trace.localRelease(origin, start);
    ++origin.generation;
    trace.localRelease(origin, start + 1ms);
    CHECK(nlohmann::json::parse(trace.report(true))["release_sample_count"] == 1);
    for (u32 index = 0; index < 515; ++index) {
        origin.emission = 0x00010000U | index;
        trace.localRelease(origin, start + 2ms);
    }
    auto report = nlohmann::json::parse(trace.report(true));
    CHECK(report["release_sample_count"] == 512);
    CHECK(report["pending_releases"] == 512);
    CHECK(report["expired_releases"] == 1);
    trace.update(start + 2s);
    origin.emission = 0x00010000;
    trace.localRelease(origin, start + 3s);
    --origin.tick;
    trace.localRelease(origin, start + 3s);
    CHECK(nlohmann::json::parse(trace.report(true))["release_sample_count"] == 512);
    origin.tick = 15;
    trace.localRelease(origin, start + 3s);
    CHECK(nlohmann::json::parse(trace.report(true))["release_sample_count"] == 513);
    trace.begin(2);
    report = nlohmann::json::parse(trace.report(true));
    CHECK(report["release_sample_count"] == 0);
    CHECK(report["expired_releases"] == 0);
}

TEST_CASE("projectile observations exclude unrelated hidden and stale release identities",
          "[netplay][latency][rollback]") {
    const auto source = GENERATE(ProjectileSource::PlayerEffect, ProjectileSource::PlayerRider);
    LatencyTelemetry trace;
    trace.begin(1);
    auto host = projectileState();
    const LatencyTelemetry::Time start{};
    trace.checkpoint(host, start);
    const ShotIdentity origin{1, 14, 3, 4, 1, false};
    trace.localRelease(origin, start + 1ms);
    for (usize field = 0; field < 12; ++field) {
        auto shown = host;
        auto other = projectile(origin);
        switch (field) {
        case 0: other.rollback.reset(); break; // familiars are not ordinary replay releases
        case 1: other.source = ProjectileSource::Enemy; break;
        case 2: other.source = ProjectileSource::Streak; break; // trail alone is not a body
        case 3: other.alpha = 0; break;
        case 4: ++other.rollback->origin.epoch; break;
        case 5: --other.rollback->origin.tick; break;
        case 6: ++other.rollback->origin.generation; break;
        case 7: ++other.rollback->origin.grant; break;
        case 8:
            other.rollback->origin.seat = 2;
            other.rollback->origin.grant = 7;
            break;
        case 9: other.rollback->origin.strong = true; break;
        case 10: other.resource = 0; break;
        case 11: other.rollback->flight.continuity = 0; break;
        default: FAIL("Unknown projectile mutation"); break;
        }
        shown.projectiles.push_back(other);
        trace.presented(shown, start + 10ms);
        CHECK(nlohmann::json::parse(trace.report())["release_to_first_projectile_frame"]["count"] ==
              0);
    }
    host.motion.tick = 9;
    host.projectiles.push_back(projectile(origin));
    trace.checkpoint(host, start + 20ms); // reordered authority cannot confirm a future release
    auto report = nlohmann::json::parse(trace.report());
    CHECK(report["release_to_host_projectile"]["count"] == 0);
    CHECK(report["pending_releases"] == 1);
    auto shown = projectileState();
    auto effect = projectile(origin);
    effect.source = source;
    effect.instance = 2000; // a pool ID is not the release identity
    shown.projectiles.push_back(effect);
    trace.presented(shown, start + 31ms);
    CHECK(nlohmann::json::parse(trace.report())["release_to_first_projectile_frame"]["p95_ms"] ==
          30);
}

TEST_CASE("release diagnostics pair both observation orders by exact shot in registration order",
          "[netplay][latency][rollback]") {
    LatencyTelemetry trace;
    trace.begin(1);
    auto host = projectileState();
    const LatencyTelemetry::Time start{};
    trace.checkpoint(host, start);
    const ShotIdentity first{1, 14, 3, 4, 1, false};
    const ShotIdentity second{1, 15, 8, 7, 2, true};
    trace.localRelease(first, start + 10ms);
    trace.localRelease(second, start + 20ms);
    auto shown = host;
    shown.projectiles = {projectile(first)};
    trace.presented(shown, start + 30ms);
    host.motion.tick = 15;
    host.projectiles = {projectile(first), projectile(second)};
    trace.checkpoint(host, start + 50ms);
    auto report = nlohmann::json::parse(trace.report(true));
    REQUIRE(report["release_samples"].size() == 2);
    CHECK(report["release_samples"][0]["outcome"] == "complete");
    CHECK(report["release_samples"][1]["outcome"] == "pending");
    CHECK(report["release_samples"][1]["first_frame_ms"].is_null());
    CHECK(report["release_samples"][1]["host_ms"] == 30);
    shown.projectiles = {projectile(second)};
    trace.presented(shown, start + 70ms);
    trace.presented(shown, start + 80ms);
    ++host.motion.tick;
    trace.checkpoint(host, start + 90ms);
    trace.localRelease(first, start + 100ms);
    trace.localRelease(second, start + 100ms);
    report = nlohmann::json::parse(trace.report(true));
    CHECK(report["release_sample_count"] == 2);
    REQUIRE(report["release_samples"].size() == 2);
    const auto& samples = report["release_samples"];
    CHECK(samples[0] == nlohmann::json{{"ordinal", 1},
                                       {"epoch", 1},
                                       {"tick", 14},
                                       {"generation", 3},
                                       {"grant", 4},
                                       {"seat", 1},
                                       {"strong", false},
                                       {"fan_ordinal", 0},
                                       {"emission", 0},
                                       {"release_monotonic_ms", 10},
                                       {"first_frame_ms", 20},
                                       {"host_ms", 40},
                                       {"outcome", "complete"}});
    CHECK(samples[1] == nlohmann::json{{"ordinal", 2},
                                       {"epoch", 1},
                                       {"tick", 15},
                                       {"generation", 8},
                                       {"grant", 7},
                                       {"seat", 2},
                                       {"strong", true},
                                       {"fan_ordinal", 0},
                                       {"emission", 0},
                                       {"release_monotonic_ms", 20},
                                       {"first_frame_ms", 50},
                                       {"host_ms", 30},
                                       {"outcome", "complete"}});
    CHECK(report["release_to_first_projectile_frame"]["count"] == 2);
    CHECK(report["release_to_host_projectile"]["count"] == 2);
    CHECK(report["pending_releases"] == 0);
}

TEST_CASE("release diagnostics preserve partial timings and terminal failure reasons",
          "[netplay][latency][rollback]") {
    const LatencyTelemetry::Time start{};
    for (const std::string outcome : {"expired", "interrupted", "unobserved"}) {
        CAPTURE(outcome);
        LatencyTelemetry trace;
        trace.begin(1);
        auto host = projectileState();
        trace.checkpoint(host, start);
        const ShotIdentity origin{1, 14, 3, 4, 1, false};
        trace.localRelease(origin, start + 1ms);
        auto shown = host;
        shown.projectiles = {projectile(origin)};
        trace.presented(shown, start + 5ms);
        if (outcome == "expired") {
            trace.update(start + 1002ms);
        } else if (outcome == "interrupted") {
            ++host.motion.tick;
            ++host.motion.players[1]->continuity;
            trace.checkpoint(host, start + 30ms);
        } else {
            host.motion.tick = origin.tick;
            trace.checkpoint(host, start + 30ms);
        }
        trace.localRelease(origin, start + 1100ms);
        trace.presented(shown, start + 1200ms);
        const auto report = nlohmann::json::parse(trace.report(true));
        REQUIRE(report["release_samples"].size() == 1);
        CHECK(report["release_sample_count"] == 1);
        CHECK(report["release_samples"][0]["first_frame_ms"] == 4);
        CHECK(report["release_samples"][0]["host_ms"].is_null());
        CHECK(report["release_samples"][0]["outcome"] == outcome);
        CHECK(report["pending_releases"] == 0);
        CHECK(report["release_to_first_projectile_frame"]["count"] == 1);
        CHECK(report["release_to_host_projectile"]["count"] == 0);
    }
}

TEST_CASE("completed release diagnostics retain a bounded window without losing aggregates",
          "[netplay][latency][rollback]") {
    LatencyTelemetry trace;
    trace.begin(1);
    auto host = projectileState();
    const LatencyTelemetry::Time start{};
    trace.checkpoint(host, start);
    for (u64 index = 0; index < 550; ++index) {
        const ShotIdentity origin{1, host.motion.tick + 1, index + 1, 4, 1, false};
        const auto now = start + std::chrono::milliseconds(index * 10);
        trace.localRelease(origin, now);
        host.projectiles = {projectile(origin)};
        trace.presented(host, now + 1ms);
        host.motion.tick = origin.tick;
        trace.checkpoint(host, now + 2ms);
    }
    const auto compact = nlohmann::json::parse(trace.report());
    CHECK_FALSE(compact.contains("release_sample_count"));
    CHECK_FALSE(compact.contains("release_samples"));
    auto report = nlohmann::json::parse(trace.report(true));
    CHECK(report["release_sample_count"] == 550);
    REQUIRE(report["release_samples"].size() == 512);
    CHECK(report["release_samples"].front()["ordinal"] == 39);
    CHECK(report["release_samples"].back()["ordinal"] == 550);
    CHECK(report["release_samples"].front()["tick"] == 49);
    CHECK(report["release_samples"].back()["tick"] == 560);
    for (const auto& sample : report["release_samples"]) {
        CHECK(sample["first_frame_ms"] == 1);
        CHECK(sample["host_ms"] == 2);
        CHECK(sample["outcome"] == "complete");
    }
    CHECK(report["release_to_first_projectile_frame"]["count"] == 550);
    CHECK(report["release_to_host_projectile"]["count"] == 550);
    CHECK(report["pending_releases"] == 0);
    report.erase("release_sample_count");
    report.erase("release_samples");
    CHECK(report == compact);
}

TEST_CASE("a passed release deadline without its shot is not projectile confirmation",
          "[netplay][latency][rollback]") {
    LatencyTelemetry trace;
    trace.begin(1);
    auto host = projectileState();
    const LatencyTelemetry::Time start{};
    trace.checkpoint(host, start);
    const ShotIdentity origin{1, 14, 3, 4, 1, false};
    trace.localRelease(origin, start + 1ms);
    host.motion.tick = 14;
    host.players[1]->animation.action = static_cast<u32>(PlayerAnimator::Action::ThrowRelease);
    host.players[1]->rollbackInput = consumedInput(host, host.motion.tick);
    trace.checkpoint(host, start + 100ms);
    auto report = nlohmann::json::parse(trace.report());
    CHECK(report["release_to_host_projectile"]["count"] == 0);
    CHECK(report["unobserved_host_projectiles"] == 1);
    CHECK(report["pending_releases"] == 0);
    host.projectiles.push_back(projectile(origin));
    trace.localRelease(origin, start + 110ms);
    trace.presented(host, start + 120ms);
    ++host.motion.tick;
    trace.checkpoint(host, start + 130ms);
    report = nlohmann::json::parse(trace.report());
    CHECK(report["release_to_first_projectile_frame"]["count"] == 0);
    CHECK(report["release_to_host_projectile"]["count"] == 0);
}

TEST_CASE("projectile telemetry clears revoked owners dead players and travel cuts",
          "[netplay][latency][rollback]") {
    const LatencyTelemetry::Time start{};
    for (usize field = 0; field < 5; ++field) {
        LatencyTelemetry trace;
        trace.begin(1);
        auto host = projectileState();
        trace.checkpoint(host, start);
        const ShotIdentity origin{1, 14, 3, 4, 1, false};
        trace.localRelease(origin, start + 1ms);
        ++host.motion.tick;
        switch (field) {
        case 0: ++host.motion.players[1]->grant; break;
        case 1: ++host.motion.players[1]->continuity; break;
        case 2: host.players[1]->life = ReplicaPlayerLife::Dying; break;
        case 3: host.players[1]->portalPhase = 0.5f; break;
        case 4: host.players[1].reset(); break;
        default: FAIL("Unknown owner mutation"); break;
        }
        host.projectiles.push_back(projectile(origin));
        trace.checkpoint(host, start + 20ms);
        trace.presented(host, start + 30ms);
        const auto report = nlohmann::json::parse(trace.report());
        CHECK(report["interrupted_releases"] == 1);
        CHECK(report["pending_releases"] == 0);
        CHECK(report["release_to_first_projectile_frame"]["count"] == 0);
        CHECK(report["release_to_host_projectile"]["count"] == 0);
    }
}

TEST_CASE("projectile release probes reject preexisting identity and retain bounded rewind memory",
          "[netplay][latency][rollback]") {
    LatencyTelemetry trace;
    trace.begin(1);
    const LatencyTelemetry::Time start{};
    auto host = projectileState();
    const ShotIdentity origin{1, 14, 3, 4, 1, false};
    trace.localRelease(origin, start); // no known authoritative owner
    CHECK(nlohmann::json::parse(trace.report())["pending_releases"] == 0);
    trace.checkpoint(host, start);
    for (usize field = 0; field < 5; ++field) {
        auto invalid = origin;
        switch (field) {
        case 0: invalid.tick = host.motion.tick; break;
        case 1: ++invalid.epoch; break;
        case 2: ++invalid.grant; break;
        case 3: invalid.seat = 4; break;
        case 4: invalid.generation = 0; break;
        default: FAIL("Unknown release mutation"); break;
        }
        trace.localRelease(invalid, start);
    }
    CHECK(nlohmann::json::parse(trace.report())["pending_releases"] == 0);
    for (u64 tick = 11; tick <= 610; ++tick) {
        auto release = origin;
        release.tick = tick;
        trace.localRelease(release, start + 1ms);
    }
    auto report = nlohmann::json::parse(trace.report(true));
    CHECK(report["pending_releases"] == 512);
    CHECK(report["expired_releases"] == 88);
    CHECK(report["release_sample_count"] == 600);
    REQUIRE(report["release_samples"].size() == 512);
    CHECK(report["release_samples"].front()["ordinal"] == 89);
    CHECK(report["release_samples"].front()["tick"] == 99);
    CHECK(report["release_samples"].front()["outcome"] == "pending");
    trace.update(start + 2s);
    report = nlohmann::json::parse(trace.report(true));
    CHECK(report["pending_releases"] == 0);
    CHECK(report["expired_releases"] == 600);
    CHECK(report["release_samples"].front()["outcome"] == "expired");
    CHECK(report["release_samples"].front()["first_frame_ms"].is_null());
    CHECK(report["release_samples"].front()["host_ms"].is_null());
    auto duplicate = origin;
    duplicate.tick = 610;
    trace.localRelease(duplicate, start + 2100ms);
    ++duplicate.generation; // a replay correction at the same tick is not a fresh observation
    trace.localRelease(duplicate, start + 2200ms);
    trace.localRelease(origin, start + 2300ms);
    CHECK(nlohmann::json::parse(trace.report())["pending_releases"] == 0);
    ++duplicate.tick;
    trace.localRelease(duplicate, start + 2400ms);
    CHECK(nlohmann::json::parse(trace.report())["pending_releases"] == 1);
    trace.begin(2);
    trace.update(start + 4s);
    report = nlohmann::json::parse(trace.report(true));
    CHECK(report["pending_releases"] == 0);
    CHECK(report["expired_releases"] == 0);
    CHECK(report["release_to_first_projectile_frame"]["count"] == 0);
    CHECK(report["release_to_host_projectile"]["count"] == 0);
    CHECK(report["release_sample_count"] == 0);
    CHECK(report["release_samples"].empty());
}
} // namespace
