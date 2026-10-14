// Native, independently paced host/guest processes. The renderer runs the real
// presentation code but discards GPU commands. Audio is not initialized: these
// timings measure visual submission, not sound output or monitor scanout.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>

#include <SDL3/SDL_timer.h>
#include <nlohmann/json.hpp>

#include "engine/core/Types.h"
#include "engine/net/SessionTransport.h"

#include "FakeRenderDevice.h"
#include "LatencyProbeEligibility.h"
#include "fixtures/MeasuredPacketTransport.h"
#include "game/combat/DamageTypes.h"
#include "game/netplay/AssetDigest.h"
#include "game/netplay/HostedRoom.h"
#include "game/players/ClassData.h"
#include "game/players/PlayerAnimator.h"
#include "game/screens/OnlinePlay.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Clock = std::chrono::steady_clock;

struct Options {
    bool host = false;
    bool internet = false;
    bool prediction = true;
    bool holdAfterReport = false;
    bool requireVolleys = false;
    std::string workload = "throw";
    std::string element = "none";
    std::string stage = "tower";
    s32 players = 2;
    s32 seconds = 20;
    s32 fps = 60;
    s32 character = 0;
    std::filesystem::path invitation;
    std::filesystem::path report;
    std::filesystem::path assets = GDL_TEST_ASSET_DIR;
    std::filesystem::path data = GDL_TEST_DATA_DIR;
};
void require(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}
u32 elementFlag(std::string_view name) {
    if (name == "fire") {
        return damage::kFire;
    }
    if (name == "lightning") {
        return damage::kElectric;
    }
    if (name == "light") {
        return damage::kLight;
    }
    if (name == "acid") {
        return damage::kAcid;
    }
    return 0;
}
Options parse(std::span<char*> args) {
    require(args.size() >= 4,
            "Usage: netplayprobe host|guest invitation-file report.json "
            "[--internet] [--prediction on|off] [--fps 30|60|144] "
            "[--seconds 20] [--players 2] [--assets path] [--data path] "
            "[--workload throw|rapid-five|speed-rapid-five|turbo-B|turbo-C|shove] "
            "[--element none|fire|lightning|light|acid] "
            "[--stage tower|G1] "
            "[--character 0..7] [--require-volleys] "
            "[--hold-after-report]");
    Options result;
    require(std::string_view(args[1]) == "host" || std::string_view(args[1]) == "guest",
            "Role must be host or guest");
    result.host = std::string_view(args[1]) == "host";
    result.invitation = args[2];
    result.report = args[3];
    for (usize i = 4; i < args.size(); ++i) {
        const std::string_view key = args[i];
        if (key == "--internet") {
            result.internet = true;
            continue;
        }
        if (key == "--hold-after-report") {
            result.holdAfterReport = true;
            continue;
        }
        if (key == "--require-volleys") {
            result.requireVolleys = true;
            continue;
        }
        require(i + 1 < args.size(), "Missing option value");
        const std::string value = args[++i];
        if (key == "--prediction") {
            require(value == "on" || value == "off", "Prediction must be on or off");
            result.prediction = value == "on";
        } else if (key == "--fps") {
            result.fps = std::stoi(value);
        } else if (key == "--seconds") {
            result.seconds = std::stoi(value);
        } else if (key == "--players") {
            result.players = std::stoi(value);
        } else if (key == "--character") {
            result.character = std::stoi(value);
        } else if (key == "--assets") {
            result.assets = value;
        } else if (key == "--data") {
            result.data = value;
        } else if (key == "--workload") {
            require(value == "throw" || value == "turbo-B" || value == "turbo-C" ||
                        value == "shove" || value == "rapid-five" || value == "speed-rapid-five",
                    "Invalid workload");
            result.workload = value;
        } else if (key == "--element") {
            require(value == "none" || value == "fire" || value == "lightning" ||
                        value == "light" || value == "acid",
                    "Invalid element");
            result.element = value;
        } else if (key == "--stage") {
            require(value == "tower" || value == "G1", "Stage must be tower or G1");
            result.stage = value;
        } else {
            throw std::runtime_error("Unknown option");
        }
    }
    require(result.fps == 30 || result.fps == 60 || result.fps == 144, "Invalid frame rate");
    require(result.players >= 2 && result.players <= 4, "Two to four players required");
    require(result.character >= 0 && result.character < kStartingClassCount,
            "Character must be a base-class index 0..7");
    require(!result.requireVolleys || result.workload == "turbo-B" || result.workload == "turbo-C",
            "Volley observations require turbo-B or turbo-C");
    require(result.seconds >= 8 && result.seconds <= 300, "Duration must be 8..300 seconds");
    require(result.workload == "throw" || result.workload == "rapid-five" ||
                result.workload == "speed-rapid-five" || result.seconds >= 20,
            "Turbo workloads need at least twenty seconds");
    require(result.element == "none" || result.workload == "throw" ||
                result.workload == "rapid-five" || result.workload == "speed-rapid-five",
            "Elemental amulets require a throw workload");
    require(!std::filesystem::exists(result.report), "Report exists; choose a new path");
    require(!result.host || !std::filesystem::exists(result.invitation),
            "Invitation exists; choose a new path");
    return result;
}
std::string readInvitation(const std::filesystem::path& path) {
    require(std::filesystem::is_regular_file(path) && std::filesystem::file_size(path) <= 2048,
            "Invalid invitation file");
    std::ifstream file(path);
    std::string value;
    std::getline(file, value);
    require(value.starts_with("gdl1-"), "Invalid invitation");
    return value;
}
void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text << '\n';
    file.close();
    require(!file.fail(), "Could not write output");
}
/** Request windows classify observations, not exact input acceptance or hit acknowledgements.
 * Keep typed bounded records during play; JSON formatting happens only in the final report. */
struct ThrowDiagnostics {
    static constexpr usize kRequests = 256;
    static constexpr usize kObservations = 64;
    static constexpr usize kOrigins = 64;
    static constexpr auto kWindow = std::chrono::seconds(1);
    struct Observation {
        u64 tick = 0;
        u64 generation = 0;
        u64 actualInput = 0;
        u64 receivedInput = 0;
        u32 action = 0;
        u32 reaction = 0;
        u32 lateInputs = 0;
        f64 elapsedMs = 0;
        bool rollback = false;
        bool enabled = false;
        bool current = false;
        bool ranged = false;
        bool weapon = false;
        bool melee = false;
        bool turbo = false;
        bool released = false;
    };
    struct Origin {
        ShotIdentity identity;
        u64 observedTick = 0;
        f64 elapsedMs = 0;
        bool live = false;
        bool tail = false;
    };
    struct Request {
        InputCommand input;
        Clock::time_point pressed;
        Clock::time_point sampleObserved;
        u64 updatesToSample = 0;
        Observation shownSeed;
        Observation hostSeed;
        u32 continuity = 0;
        std::optional<Observation> firstFrame;
        std::optional<Observation> hostAction;
        std::vector<Observation> host;
        std::vector<Origin> origins;
        u64 lastTick = 0;
        u64 omittedObservations = 0;
        u64 omittedOrigins = 0;
        bool closed = false;
        bool interrupted = false;
    };
    std::vector<Request> requests;
    u64 omitted = 0;
    struct Physical {
        u64 instance = 0;
        u64 snapshotTick = 0;
        s32 owner = 0;
        f32 age = 0;
        std::optional<MissileRelease> release;
    };
    static constexpr usize kPhysical = 4096;
    std::vector<Physical> physical;
    u64 omittedPhysical = 0;

    void observePhysical(const PlayScene* scene, const CombatSnapshot* shown) {
        if (scene == nullptr) {
            return;
        }
        const auto& missiles = scene->missiles();
        for (usize index = 0; index < missiles.count(); ++index) {
            const auto& missile = missiles.missile(index);
            if (std::ranges::find(physical, missile.instance, &Physical::instance) !=
                physical.end()) {
                continue;
            }
            if (physical.size() == kPhysical) {
                ++omittedPhysical;
                continue;
            }
            physical.push_back({missile.instance, shown ? shown->motion.tick : 0, missile.owner,
                                missile.age, missile.release});
        }
    }

    static Observation observation(const CombatSnapshot& state, u8 seat, bool native,
                                   f64 elapsedMs = 0) {
        Observation result;
        result.tick = state.motion.tick;
        result.elapsedMs = elapsedMs;
        if (seat >= state.players.size() || !state.players[seat]) {
            return result;
        }
        const auto& player = *state.players[seat];
        result.rollback = player.rollback.has_value();
        result.action = native && player.rollback ? static_cast<u32>(player.rollback->action)
                                                  : player.animation.action;
        result.generation = native && player.rollback ? player.rollback->animation.generation
                                                      : player.animation.generation;
        result.actualInput = player.rollbackInput ? player.rollbackInput->tick : 0;
        result.receivedInput = player.movement.receivedInput;
        result.lateInputs = player.movement.lateInputs;
        result.reaction = static_cast<u32>(player.reaction);
        result.enabled = player.movement.enabled;
        result.current = player.movement.current;
        result.ranged = player.movement.ranged;
        result.weapon = player.weapon.has_value();
        result.melee = player.melee.has_value();
        result.turbo = player.turbo.has_value();
        result.released = player.rollback && player.rollback->released;
        return result;
    }
    void bind(const InputCommand& input, const CombatSnapshot& shown, const Observation& hostSeed,
              Clock::time_point pressed, u64 updatesToSample) {
        if (requests.size() == kRequests) {
            ++omitted;
            return;
        }
        Request request;
        request.input = input;
        request.pressed = pressed;
        request.sampleObserved = Clock::now();
        request.updatesToSample = updatesToSample;
        request.shownSeed = observation(shown, input.seat, false);
        request.hostSeed = hostSeed;
        request.continuity = shown.motion.players[input.seat]->continuity;
        request.host.reserve(kObservations);
        request.origins.reserve(kOrigins);
        requests.push_back(std::move(request));
    }
    void observe(const CombatSnapshot* state, Clock::time_point now) {
        for (auto& request : requests) {
            if (request.closed) {
                continue;
            }
            if (now - request.pressed > kWindow) {
                request.closed = true;
                continue;
            }
            if (state == nullptr) {
                continue;
            }
            const auto& input = request.input;
            if (state->motion.epoch != input.epoch || !state->motion.players[input.seat] ||
                state->motion.players[input.seat]->grant != input.grant ||
                state->motion.players[input.seat]->continuity != request.continuity ||
                !state->players[input.seat] ||
                state->players[input.seat]->life != ReplicaPlayerLife::Standing ||
                state->players[input.seat]->portalPhase) {
                request.closed = request.interrupted = true;
                continue;
            }
            if (state->motion.tick < input.tick || state->motion.tick <= request.lastTick) {
                continue;
            }
            request.lastTick = state->motion.tick;
            const auto elapsed =
                std::chrono::duration<f64, std::milli>(now - request.pressed).count();
            const auto observed = observation(*state, input.seat, true, elapsed);
            if (request.host.size() < kObservations) {
                request.host.push_back(observed);
            } else {
                ++request.omittedObservations;
            }
            if (!request.hostAction && attack(observed.action) &&
                observed.generation > request.hostSeed.generation &&
                observed.actualInput >= input.tick && observed.actualInput <= observed.tick) {
                request.hostAction = observed;
            }
            for (const auto& shot : state->projectiles) {
                const auto* origin = shot.releaseOrigin();
                if (origin == nullptr || origin->epoch != input.epoch ||
                    origin->seat != input.seat || origin->grant != input.grant ||
                    origin->tick < input.tick) {
                    continue;
                }
                auto found = std::ranges::find(request.origins, *origin, &Origin::identity);
                if (found == request.origins.end()) {
                    if (request.origins.size() == kOrigins) {
                        ++request.omittedOrigins;
                        continue;
                    }
                    request.origins.push_back({*origin, state->motion.tick, elapsed});
                    found = std::prev(request.origins.end());
                }
                found->live |= shot.rollback.has_value();
                found->tail |= shot.particles && shot.particles->flight.retiring;
            }
        }
    }
    static bool attack(u32 action) {
        using Action = PlayerAnimator::Action;
        const auto value = static_cast<Action>(action);
        return (value >= Action::Throw && value <= Action::ThrowMovingRecover) ||
               (value >= Action::Quick1 && value <= Action::PowerLowRecover);
    }
    void presented(const CombatSnapshot* state, Clock::time_point now) {
        if (state == nullptr) {
            return;
        }
        for (auto& request : requests) {
            if (request.closed || request.firstFrame) {
                continue;
            }
            if (now - request.pressed > kWindow) {
                request.closed = true;
                continue;
            }
            const auto& input = request.input;
            const auto& pose = state->motion.players[input.seat];
            const auto& player = state->players[input.seat];
            if (state->motion.epoch != input.epoch || !pose || !player ||
                pose->grant != input.grant || pose->continuity != request.continuity ||
                player->life != ReplicaPlayerLife::Standing || player->portalPhase) {
                request.closed = request.interrupted = true;
                continue;
            }
            const auto observed =
                observation(*state, input.seat, false,
                            std::chrono::duration<f64, std::milli>(now - request.pressed).count());
            if (attack(observed.action) && observed.generation > request.shownSeed.generation) {
                request.firstFrame = observed;
            }
        }
    }
    static nlohmann::json encode(const Observation& value) {
        using Action = PlayerAnimator::Action;
        const auto action = static_cast<Action>(value.action);
        const char* family = "other";
        if (action >= Action::Throw && action <= Action::ThrowMovingRecover) {
            family = "throw";
        } else if (action >= Action::Quick1 && action <= Action::PowerLowRecover) {
            family = "melee";
        } else if (action <= Action::Idle2Loop) {
            family = "idle";
        }
        return {{"tick", value.tick},
                {"action", value.action},
                {"action_family", family},
                {"generation", value.generation},
                {"elapsed_ms", value.elapsedMs},
                {"rollback", value.rollback},
                {"actual_input_tick", value.actualInput},
                {"received_input_tick", value.receivedInput},
                {"late_inputs", value.lateInputs},
                {"reaction", value.reaction},
                {"movement_enabled", value.enabled},
                {"movement_current", value.current},
                {"ranged", value.ranged},
                {"weapon", value.weapon},
                {"melee", value.melee},
                {"turbo", value.turbo},
                {"released", value.released}};
    }
    void append(nlohmann::json& report) const {
        auto records = nlohmann::json::array();
        LatencyTelemetry::Distribution frames;
        LatencyTelemetry::Distribution authority;
        usize complete = 0;
        usize pending = 0;
        usize interrupted = 0;
        usize expired = 0;
        for (const auto& request : requests) {
            if (request.firstFrame) {
                frames.add(request.firstFrame->elapsedMs);
            }
            if (request.hostAction) {
                authority.add(request.hostAction->elapsedMs);
            }
            if (request.firstFrame && request.hostAction) {
                ++complete;
            } else if (request.interrupted) {
                ++interrupted;
            } else if (request.closed) {
                ++expired;
            } else {
                ++pending;
            }
            auto host = nlohmann::json::array();
            for (const auto& value : request.host) {
                host.push_back(encode(value));
            }
            auto origins = nlohmann::json::array();
            for (const auto& value : request.origins) {
                const auto& id = value.identity;
                origins.push_back({{"epoch", id.epoch},
                                   {"tick", id.tick},
                                   {"generation", id.generation},
                                   {"seat", id.seat},
                                   {"grant", id.grant},
                                   {"strong", id.strong},
                                   {"ordinal", id.ordinal},
                                   {"emission", id.emission},
                                   {"observed_tick", value.observedTick},
                                   {"elapsed_ms", value.elapsedMs},
                                   {"live", value.live},
                                   {"retiring_tail", value.tail}});
            }
            records.push_back(
                {{"epoch", request.input.epoch},
                 {"tick", request.input.tick},
                 {"seat", request.input.seat},
                 {"grant", request.input.grant},
                 {"sampled_held_buttons", request.input.heldButtons},
                 {"sampled_pressed_buttons", request.input.pressedButtons},
                 {"request_monotonic_ms",
                  std::chrono::duration<f64, std::milli>(request.pressed.time_since_epoch())
                      .count()},
                 {"sample_observed_monotonic_ms",
                  std::chrono::duration<f64, std::milli>(request.sampleObserved.time_since_epoch())
                      .count()},
                 {"sample_observed_delay_ms",
                  std::chrono::duration<f64, std::milli>(request.sampleObserved - request.pressed)
                      .count()},
                 {"updates_to_sample", request.updatesToSample},
                 {"shown_seed", encode(request.shownSeed)},
                 {"host_seed", encode(request.hostSeed)},
                 {"first_attack_frame",
                  request.firstFrame ? encode(*request.firstFrame) : nlohmann::json(nullptr)},
                 {"host_attack_action",
                  request.hostAction ? encode(*request.hostAction) : nlohmann::json(nullptr)},
                 {"closed", request.closed},
                 {"interrupted", request.interrupted},
                 {"host_observations", std::move(host)},
                 {"projectile_origins", std::move(origins)},
                 {"omitted_observations", request.omittedObservations},
                 {"omitted_origins", request.omittedOrigins}});
        }
        report["throw_diagnostics"] = {
            {"scope",
             "post-request observations; not exact input acceptance or hit acknowledgements"},
            {"admission_trace_schema", 1},
            {"admission_trace_scope",
             "actual sampled command; observed after native update including replay; "
             "not the exact sample/send instant or host acceptance"},
            {"window_ms", 1000},
            {"omitted_requests", omitted},
            {"requests", std::move(records)}};
        const auto distribution = [](const LatencyTelemetry::Distribution& values) {
            const auto summary = values.summary();
            return nlohmann::json{{"count", summary.count},      {"window", summary.window},
                                  {"median_ms", summary.median}, {"p95_ms", summary.p95},
                                  {"p99_ms", summary.p99},       {"max_ms", summary.maximum}};
        };
        report["attack_probe_schema"] = 1;
        report["attack_observation_scope"] =
            "new ordinary melee or throw action after an idle request; not an input or hit ACK";
        report["attack_to_first_frame"] = distribution(frames);
        report["attack_to_host_action"] = distribution(authority);
        report["complete_attack_requests"] = complete;
        report["pending_attack_requests"] = pending;
        report["interrupted_attack_requests"] = interrupted;
        report["expired_attack_requests"] = expired;
        auto native = nlohmann::json::array();
        for (const auto& value : physical) {
            nlohmann::json record = {{"instance", value.instance},
                                     {"observed_snapshot_tick", value.snapshotTick},
                                     {"owner", value.owner},
                                     {"age", value.age},
                                     {"weapon_release", value.release.has_value()}};
            if (value.release) {
                record["generation"] = value.release->generation;
                record["ordinal"] = value.release->ordinal;
                record["emission"] = value.release->emission;
                record["strong"] = value.release->strong;
            }
            native.push_back(std::move(record));
        }
        report["native_projectile_diagnostics"] = {
            {"scope", "host physical pool first observations; not input acknowledgements; "
                      "same-update impacts may be absent"},
            {"omitted_observations", omittedPhysical},
            {"projectiles", std::move(native)}};
    }
};

/** Sparse turbo observations are action-start evidence, not accepted-input or hit
 * acknowledgements. Both clocks live only in this guest process. */
struct TurboDiagnostics {
    enum class Outcome : u8 { Pending, Complete, Expired, Interrupted };
    static constexpr usize kSamples = 128;
    static constexpr auto kTimeout = std::chrono::seconds(2);
    struct Sample {
        Clock::time_point pressed;
        u64 epoch = 0;
        u64 tick = 0;
        u64 hostTick = 0;
        u64 shownGeneration = 0;
        u64 hostGeneration = 0;
        u8 seat = 0;
        SeatMotion origin;
        PlayerAnimator::Action action{};
        std::optional<f64> firstFrameMs;
        std::optional<f64> hostMs;
        Outcome outcome = Outcome::Pending;
        bool sampled = false;
    };
    std::array<Sample, kSamples> samples{};
    usize count = 0;
    std::optional<usize> pending;
    u64 omitted = 0;
    LatencyTelemetry::Distribution frames;
    LatencyTelemetry::Distribution authority;

    static bool ready(const CombatSnapshot* state, u8 seat, bool native,
                      PlayerAnimator::Action action) {
        if (!(native ? test::LatencyProbeEligibility::authorityIdle(state, seat)
                     : test::LatencyProbeEligibility::visualIdle(state, seat))) {
            return false;
        }
        if (!native) {
            return true; // A drawing result is not a turbo or movement certificate.
        }
        const auto& player = *state->players[seat];
        return player.turbo && player.turboGeometry && player.turboGeometry->moving &&
               player.turbo->move.owed == 0 &&
               (action == PlayerAnimator::Action::Shove
                    ? player.shove && player.turbo->meter.held >= TurboMeter::kShoveFrom
                : action == PlayerAnimator::Action::TurboFull
                    ? player.turbo->meter.held >= TurboMeter::kFullCost
                    : player.turbo->meter.held >= TurboMeter::kStrongCost &&
                          player.turbo->meter.held < TurboMeter::kFullCost);
    }
    bool arm(const CombatSnapshot& shown, const CombatSnapshot& host, u8 seat,
             PlayerAnimator::Action action, Clock::time_point now) {
        if (pending || !ready(&shown, seat, false, action) || !ready(&host, seat, true, action) ||
            shown.motion.epoch != host.motion.epoch ||
            shown.motion.players[seat]->grant != host.motion.players[seat]->grant ||
            shown.motion.players[seat]->continuity != host.motion.players[seat]->continuity) {
            return false;
        }
        if (count == samples.size()) {
            ++omitted;
            return false;
        }
        auto& sample = samples[count];
        sample.pressed = now;
        sample.epoch = host.motion.epoch;
        sample.hostTick = host.motion.tick;
        sample.shownGeneration = shown.players[seat]->animation.generation;
        const auto& player = *host.players[seat];
        sample.hostGeneration =
            player.rollback ? player.rollback->animation.generation : player.animation.generation;
        sample.seat = seat;
        sample.origin = *host.motion.players[seat];
        sample.action = action;
        pending = count++;
        return true;
    }
    void bind(std::span<const InputCommand> commands) {
        if (!pending || samples[*pending].sampled) {
            return;
        }
        auto& sample = samples[*pending];
        for (const auto& command : commands) {
            if (command.valid() && command.epoch == sample.epoch && command.seat == sample.seat &&
                command.grant == sample.origin.grant && command.tick > sample.hostTick &&
                command.pressed(sample.action == PlayerAnimator::Action::Shove
                                    ? CommandPress::Charge
                                    : CommandPress::TurboAttack)) {
                sample.tick = command.tick;
                sample.sampled = true;
                break;
            }
        }
    }
    void observe(const CombatSnapshot* state, bool host, Clock::time_point now) {
        if (!pending || state == nullptr) {
            return;
        }
        auto& sample = samples[*pending];
        if (now - sample.pressed > kTimeout) {
            sample.outcome = Outcome::Expired;
            pending.reset();
            return;
        }
        const auto& pose = state->motion.players[sample.seat];
        const auto& player = state->players[sample.seat];
        if (state->motion.epoch != sample.epoch || !pose || !player ||
            pose->grant != sample.origin.grant || pose->continuity != sample.origin.continuity ||
            player->life != ReplicaPlayerLife::Standing || player->portalPhase) {
            sample.outcome = Outcome::Interrupted;
            pending.reset();
            return;
        }
        if (!sample.sampled || (host && state->motion.tick <= sample.hostTick)) {
            return;
        }
        const auto action = host && player->rollback
                                ? player->rollback->action
                                : static_cast<PlayerAnimator::Action>(player->animation.action);
        const auto generation = host && player->rollback ? player->rollback->animation.generation
                                                         : player->animation.generation;
        if (host) {
            sample.hostTick = state->motion.tick;
        }
        if ((PlayerAnimator::rollbackTurboAction(action) ||
             action == PlayerAnimator::Action::Shove) &&
            (action != sample.action || (host && state->motion.tick < sample.tick))) {
            sample.outcome = Outcome::Interrupted;
            pending.reset();
            return;
        }
        if (action == sample.action) {
            const f64 elapsed =
                std::chrono::duration<f64, std::milli>(now - sample.pressed).count();
            if (!host && !sample.firstFrameMs && generation != sample.shownGeneration) {
                sample.firstFrameMs = elapsed;
                frames.add(elapsed);
            } else if (host && !sample.hostMs && generation != sample.hostGeneration) {
                const auto& consumed = player->rollbackInput;
                if (consumed && consumed->valid() && consumed->epoch == sample.epoch &&
                    consumed->seat == sample.seat && consumed->grant == sample.origin.grant &&
                    consumed->tick >= sample.tick && consumed->tick <= state->motion.tick) {
                    sample.hostMs = elapsed;
                    authority.add(elapsed);
                }
            }
        }
        if (sample.firstFrameMs && sample.hostMs) {
            sample.outcome = Outcome::Complete;
            pending.reset();
        }
    }
    void append(nlohmann::json& report) const {
        const auto distribution = [](const LatencyTelemetry::Distribution& values) {
            const auto summary = values.summary();
            return nlohmann::json{{"count", summary.count},      {"window", summary.window},
                                  {"median_ms", summary.median}, {"p95_ms", summary.p95},
                                  {"p99_ms", summary.p99},       {"max_ms", summary.maximum}};
        };
        report["turbo_probe_schema"] = 1;
        report["turbo_to_first_frame"] = distribution(frames);
        report["turbo_to_host_action"] = distribution(authority);
        report["turbo_requests"] = count;
        report["omitted_turbo_requests"] = omitted;
        constexpr std::array<std::string_view, 4> kNames{"pending", "complete", "expired",
                                                         "interrupted"};
        std::array<usize, kNames.size()> outcomes{};
        auto& records = report["turbo_samples"] = nlohmann::json::array();
        for (usize i = 0; i < count; ++i) {
            const auto& sample = samples[i];
            const auto outcome = static_cast<usize>(sample.outcome);
            ++outcomes[outcome];
            records.push_back(
                {{"epoch", sample.epoch},
                 {"seat", sample.seat},
                 {"grant", sample.origin.grant},
                 {"tick", sample.tick},
                 {"sampled", sample.sampled},
                 {"action", static_cast<u32>(sample.action)},
                 {"request_monotonic_ms",
                  std::chrono::duration<f64, std::milli>(sample.pressed.time_since_epoch())
                      .count()},
                 {"first_frame_ms", sample.firstFrameMs ? nlohmann::json(*sample.firstFrameMs)
                                                        : nlohmann::json(nullptr)},
                 {"host_action_ms",
                  sample.hostMs ? nlohmann::json(*sample.hostMs) : nlohmann::json(nullptr)},
                 {"outcome", kNames[outcome]}});
        }
        for (usize i = 0; i < kNames.size(); ++i) {
            report[std::string(kNames[i]) + "_turbo_requests"] = outcomes[i];
        }
    }
};

/** Test-only resource provisioning, never a guessed network response. The host
 * publishes this native meter before the guest can request an attack. */
usize fundTurbo(const OnlinePlay& play, bool full) {
    const auto* scene = play.hostScene();
    if (scene == nullptr || !scene->movementPredictable()) {
        return 0;
    }
    usize funded = 0;
    for (const auto& player : scene->participants()) {
        if (player.life != PlayerLife::Standing || player.figure == nullptr ||
            player.figure->animator().action() > PlayerAnimator::Action::Idle2Loop ||
            player.move.owed() != 0) {
            continue;
        }
        const f32 amount = full ? TurboMeter::kFullCost : TurboMeter::kStrongCost;
        if (player.turbo.held() < amount) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): owned test runtime
            auto& runtime = const_cast<PlayerRuntime&>(player);
            runtime.turbo.add(amount - runtime.turbo.held());
            ++funded;
        }
    }
    return funded;
}

/** Native inventory provisioning belongs only to this headless test's host. */
void equipItems(const OnlinePlay& play, bool fan, bool speed, u32 element) {
    const auto* scene = play.hostScene();
    if (scene == nullptr) {
        return;
    }
    for (const auto& player : scene->participants()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): owned test runtime
        auto& inventory = const_cast<PlayerRuntime&>(player).actor.save().progress().inventory;
        for (const u32 flag :
             {fan ? powerup::kRapidFire : 0U, fan ? powerup::kFiveWayShot : 0U, element}) {
            if (flag == 0) {
                continue;
            }
            if (inventory.powerup(powerup::kWeapon, flag) == nullptr) {
                inventory.addPowerup(powerup::kWeapon, flag, -1, 600);
            }
        }
        // Kind-7 Speed changes both native pace and animation speed. A special
        // Speed flag alone would exercise only the latter, hiding a stale pace.
        if (speed && std::ranges::none_of(inventory.powerups, [](const auto& slot) {
                return slot.working() && slot.kind == powerup::kSpeed;
            })) {
            inventory.addPowerup(powerup::kSpeed, 0, 3, 600);
        }
    }
}

/** Center-shot timings use the existing telemetry; these separate counters prove
 * all five native fan ordinals were actually received and submitted for drawing. */
struct FanDiagnostics {
    struct Speed {
        u64 certified = 0;
        u64 motions = 0;
        u64 epoch = 0;
        std::optional<SeatMotion> previous;
        std::optional<f32> minimumPace;
        std::optional<f32> maximumPace;
    };
    std::array<std::map<ShotIdentity, u8>, 2> fans;
    std::array<Speed, 2> speeds;
    u64 certified = 0;
    void observe(const CombatSnapshot* state, u8 seat, bool authority, u32 element) {
        if (state == nullptr || !state->players[seat]) {
            return;
        }
        const auto& player = *state->players[seat];
        const bool certifiedFan =
            player.modifiers && player.modifiers->ticks != 0 && player.weapon &&
            player.weapon->shots == 5 &&
            player.modifiers->flags == (powerup::kRapidFire | powerup::kFiveWayShot | element);
        if (authority && certifiedFan) {
            ++certified;
        }
        const usize index = authority ? 1 : 0;
        auto& speed = speeds[index];
        const auto& pose = state->motion.players[seat];
        if (certifiedFan && player.modifiers->speed && player.movement.enabled && pose) {
            ++speed.certified;
            const f32 pace = player.movement.speed;
            speed.minimumPace = std::min(speed.minimumPace.value_or(pace), pace);
            speed.maximumPace = std::max(speed.maximumPace.value_or(pace), pace);
            if (speed.previous && speed.epoch == state->motion.epoch &&
                speed.previous->grant == pose->grant &&
                speed.previous->continuity == pose->continuity) {
                const Vec3 delta = pose->position - speed.previous->position;
                if (delta.x * delta.x + delta.z * delta.z > 0.000001f) {
                    ++speed.motions;
                }
            }
            speed.epoch = state->motion.epoch;
            speed.previous = pose;
        } else {
            speed.previous.reset();
        }
        auto& seen = fans[index];
        for (const auto& shot : state->projectiles) {
            if ((shot.source != ProjectileSource::PlayerEffect &&
                 shot.source != ProjectileSource::PlayerRider) ||
                !shot.rollback || shot.rollback->origin.seat != seat) {
                continue;
            }
            auto identity = shot.rollback->origin;
            const u8 bit = static_cast<u8>(1U << identity.ordinal);
            identity.ordinal = 0;
            if (seen.contains(identity) || seen.size() < 512) {
                seen[identity] |= bit;
            }
        }
    }
    void append(nlohmann::json& report, bool speedWorkload) const {
        report["item_probe_schema"] = 1;
        report["projectile_timing_scope"] = "center-shot";
        report["certified_item_observations"] = certified;
        for (usize index = 0; index < fans.size(); ++index) {
            const auto complete = std::ranges::count_if(
                fans[index], [](const auto& entry) { return entry.second == 31; });
            const std::string prefix = index == 0 ? "displayed_" : "host_";
            report[prefix + "complete_fans"] = complete;
            report[prefix + "partial_fans"] = fans[index].size() - static_cast<usize>(complete);
            if (speedWorkload) {
                const auto& speed = speeds[index];
                report[prefix + "speed_certified_observations"] = speed.certified;
                report[prefix + "speed_motion_observations"] = speed.motions;
                report[prefix + "speed_minimum_pace"] = speed.minimumPace
                                                            ? nlohmann::json(*speed.minimumPace)
                                                            : nlohmann::json(nullptr);
                report[prefix + "speed_maximum_pace"] = speed.maximumPace
                                                            ? nlohmann::json(*speed.maximumPace)
                                                            : nlohmann::json(nullptr);
            }
        }
        if (speedWorkload) {
            report["speed_probe_schema"] = 1;
        }
    }
};

/** Count distinct native rider releases, not repeated frames of one sprite.
 * Resource identity comes from the same trusted host descriptor as replay. */
struct ElementDiagnostics {
    std::array<std::map<ShotIdentity, u32>, 2> projectiles;
    std::array<u64, 2> invalid{};
    u64 certified = 0;
    void observe(const CombatSnapshot* state, u8 seat, bool authority, u32 element) {
        if (state == nullptr || !state->players[seat] || !state->motion.players[seat]) {
            return;
        }
        const auto& player = *state->players[seat];
        const auto& pose = *state->motion.players[seat];
        if (!player.modifiers || !player.weapon || player.weapon->rider == 0 ||
            player.modifiers->ticks == 0 || damage::element(player.modifiers->flags) != element) {
            return;
        }
        if (authority) {
            ++certified;
        }
        const usize index = authority ? 1 : 0;
        for (const auto& shot : state->projectiles) {
            if (shot.source != ProjectileSource::PlayerRider || !shot.rollback ||
                shot.rollback->origin.seat != seat) {
                continue;
            }
            const auto& origin = shot.rollback->origin;
            if (origin.epoch != state->motion.epoch || origin.grant != pose.grant ||
                shot.resource != player.weapon->rider) {
                ++invalid[index];
                continue;
            }
            auto& seen = projectiles[index];
            if (!seen.contains(origin) && seen.size() < 512) {
                seen.emplace(origin, shot.resource);
            }
        }
    }
    void append(nlohmann::json& report, u32 element) const {
        report["element_probe_schema"] = 1;
        report["element_flags"] = element;
        report["element_certified_observations"] = certified;
        for (usize index = 0; index < projectiles.size(); ++index) {
            const std::string prefix = index == 0 ? "displayed_" : "host_";
            report[prefix + "element_projectiles"] = projectiles[index].size();
            report[prefix + "invalid_element_projectiles"] = invalid[index];
        }
    }
};

/** The two sets observe this guest's submitted and authoritative live missiles.
 * One physical release may have several visual components; count its identity
 * only once. Their intersection proves the same volley was seen on both paths,
 * not that the projectile hit an enemy or that an input was accepted. */
struct VolleyDiagnostics {
    static constexpr usize kOrigins = 1024;
    std::array<std::set<ShotIdentity>, 2> origins;
    std::array<bool, 2> overflow{};
    std::array<u64, 2> invalid{};
    void observe(const CombatSnapshot* state, u8 seat, bool authority) {
        if (state == nullptr || !state->motion.players[seat]) {
            return;
        }
        const usize index = authority ? 1 : 0;
        const auto& pose = *state->motion.players[seat];
        for (const auto& shot : state->projectiles) {
            if (!shot.rollback || shot.rollback->origin.seat != seat ||
                shot.rollback->origin.emission == 0 || shot.alpha <= 0 ||
                (shot.source != ProjectileSource::Player &&
                 shot.source != ProjectileSource::PlayerEffect &&
                 shot.source != ProjectileSource::PlayerRider)) {
                continue;
            }
            const auto& origin = shot.rollback->origin;
            if (!origin.valid() || !shot.valid() || origin.epoch != state->motion.epoch ||
                origin.grant != pose.grant) {
                ++invalid[index];
                continue;
            }
            auto& seen = origins[index];
            if (seen.contains(origin)) {
                continue;
            }
            if (seen.size() == kOrigins) {
                overflow[index] = true;
            } else {
                seen.insert(origin);
            }
        }
    }
    void append(nlohmann::json& report) const {
        report["turbo_volley_probe_schema"] = 1;
        report["projectile_timing_scope"] = "turbo-volley-release";
        for (usize index = 0; index < origins.size(); ++index) {
            const std::string prefix = index == 0 ? "displayed_" : "host_";
            report[prefix + "volley_projectiles"] = origins[index].size();
            report[prefix + "invalid_volley_projectiles"] = invalid[index];
            report[prefix + "volley_overflow"] = overflow[index];
        }
        report["matched_volley_projectiles"] = std::ranges::count_if(
            origins[0], [&](const auto& origin) { return origins[1].contains(origin); });
    }
};

/** Fixed-size local diagnostics: no output, allocation or network activity on
 * the measured path. Slow first-use work is retained, never excluded. */
struct LoopDiagnostics {
    enum class Operation : u8 { Update, Render, PlayUpdate, SessionUpdate, SceneOpen };
    static constexpr std::array<std::string_view, 5> kNames{"update", "render", "play_update",
                                                            "session_update", "scene_open"};
    static constexpr f64 kSlowMs = 30;
    struct Slow {
        Operation operation{};
        f64 startMonotonicMs = 0;
        f64 processElapsedMs = 0;
        std::optional<f64> gameplayElapsedMs;
        f64 durationMs = 0;
        OnlinePlay::Phase phase{};
    };
    std::array<f64, kNames.size()> maxima{};
    std::array<Slow, 64> slow{};
    usize count = 0;
    u64 omitted = 0;

    void record(Operation operation, Clock::time_point begin, Clock::time_point end,
                Clock::time_point processBegin, std::optional<Clock::time_point> gameplayBegin,
                OnlinePlay::Phase phase) {
        const f64 duration = std::chrono::duration<f64, std::milli>(end - begin).count();
        auto& maximum = maxima[static_cast<usize>(operation)];
        maximum = std::max(maximum, duration);
        if (duration <= kSlowMs) {
            return;
        }
        if (count == slow.size()) {
            ++omitted;
            return;
        }
        auto& entry = slow[count++];
        entry.operation = operation;
        entry.startMonotonicMs =
            std::chrono::duration<f64, std::milli>(begin.time_since_epoch()).count();
        entry.processElapsedMs =
            std::chrono::duration<f64, std::milli>(begin - processBegin).count();
        if (gameplayBegin) {
            entry.gameplayElapsedMs =
                std::chrono::duration<f64, std::milli>(begin - *gameplayBegin).count();
        }
        entry.durationMs = duration;
        entry.phase = phase;
    }
    void append(nlohmann::json& report) const {
        constexpr std::array<std::string_view, 7> kPhases{
            "closed", "loading", "playing", "paused", "shopping", "finished", "failed"};
        for (usize i = 0; i < maxima.size(); ++i) {
            report[std::string(kNames[i]) + "_max_ms"] = maxima[i];
        }
        report["slow_operation_threshold_ms"] = kSlowMs;
        report["slow_operations_omitted"] = omitted;
        auto& records = report["slow_operations"] = nlohmann::json::array();
        for (usize i = 0; i < count; ++i) {
            const auto& entry = slow[i];
            records.push_back({{"operation", kNames[static_cast<usize>(entry.operation)]},
                               {"start_monotonic_ms", entry.startMonotonicMs},
                               {"process_elapsed_ms", entry.processElapsedMs},
                               {"gameplay_elapsed_ms",
                                entry.gameplayElapsedMs ? nlohmann::json(*entry.gameplayElapsedMs)
                                                        : nlohmann::json(nullptr)},
                               {"duration_ms", entry.durationMs},
                               {"phase", kPhases[static_cast<usize>(entry.phase)]}});
        }
    }
};

/** Observe complete host snapshots, never the locally extrapolated display.
 * Fixed-size previous-state storage avoids adding per-tick profiling allocations. */
struct StageDiagnostics {
    struct Enemy {
        u64 instance = 0;
        Vec3 position{0};
        CombatAnimation animation;
    };
    std::array<Enemy, CombatSnapshot::kMaxEnemies> previous{};
    usize previousCount = 0;
    u64 epoch = 0;
    u64 tick = 0;
    u64 observations = 0;
    u64 enemyFrames = 0;
    u64 motions = 0;
    u64 animations = 0;
    u64 opponentSeeds = 0;
    u64 combatSeeds = 0;
    u64 stageSeeds = 0;
    u64 stageBytes = 0;
    usize stageBytesMax = 0;
    usize peak = 0;

    void observe(const CombatSnapshot* state) {
        if (state == nullptr || (state->motion.epoch == epoch && state->motion.tick <= tick)) {
            return;
        }
        if (state->motion.epoch != epoch) {
            previousCount = 0;
        }
        epoch = state->motion.epoch;
        tick = state->motion.tick;
        ++observations;
        opponentSeeds += state->opponents && state->opponentFrame ? 1 : 0;
        if (state->combatState && state->combatFrame) {
            ++combatSeeds;
            if (state->combatState->stage) {
                ++stageSeeds;
                const auto bytes = state->combatState->stage->size();
                stageBytes += bytes;
                stageBytesMax = std::max(stageBytesMax, bytes);
            }
        }
        usize active = 0;
        usize previousIndex = 0;
        for (const auto& enemy : state->enemies) {
            if (enemy.life != ReplicaEnemyLife::Active) {
                continue;
            }
            // Snapshot instances and the filtered previous list are ordered.
            while (previousIndex < previousCount &&
                   previous[previousIndex].instance < enemy.instance) {
                ++previousIndex;
            }
            if (previousIndex < previousCount &&
                previous[previousIndex].instance == enemy.instance) {
                const auto& before = previous[previousIndex];
                motions += before.position != enemy.position ? 1 : 0;
                animations += before.animation.action != enemy.animation.action ||
                                      before.animation.generation != enemy.animation.generation ||
                                      before.animation.frame != enemy.animation.frame
                                  ? 1
                                  : 0;
            }
            ++active;
        }
        peak = std::max(peak, active);
        enemyFrames += active != 0 ? 1 : 0;
        previousCount = 0;
        for (const auto& enemy : state->enemies) {
            if (enemy.life == ReplicaEnemyLife::Active && previousCount < previous.size()) {
                previous[previousCount++] = {enemy.instance, enemy.position, enemy.animation};
            }
        }
    }
    void append(nlohmann::json& report, bool host) const {
        report["stage_probe_schema"] = 1;
        report["stage_activity_scope"] = "authority-checkpoints";
        report["stage_authority_observations"] = observations;
        report["stage_enemy_frames"] = enemyFrames;
        report["stage_enemy_motion_samples"] = motions;
        report["stage_enemy_animation_samples"] = animations;
        report["stage_enemy_peak"] = peak;
        report["stage_opponent_seed_observations"] = opponentSeeds;
        report["combat_seeds"] = {
            {"schema", 1},
            {"scope", "distinct-authority-post-step-checkpoints"},
            {"coverage", "latest-after-update-not-every-transport-snapshot"},
            {"observation", host ? "host-captured" : "guest-received"},
            {"observations", observations},
            {"opponent", opponentSeeds},
            {"combat", combatSeeds},
            {"stage", stageSeeds},
            {"stage_bytes_total", stageBytes},
            {"stage_bytes_max", stageBytesMax},
            {"capture_nanoseconds", nullptr},
            {"encode_nanoseconds", nullptr},
            {"timing_scope", "capture-and-encode-not-instrumented-no-duplicate-work"}};
    }
};

/** Read cumulative native counters once at each measurement boundary. Counting
 * prefixes separately from accepted passes prevents a late refusal from being
 * reported as usable Stage prediction. The local movement-only lane is not in
 * these counters; legacy means a complete combat seed without Stage owners. */
nlohmann::json replayReport(const OnlineCombatReplay::Diagnostics& before,
                            const OnlineCombatReplay::Diagnostics& after) {
    u64 resets = 0;
    const auto difference = [&](u64 first, u64 last) {
        if (last < first) {
            ++resets;
            return u64{0};
        }
        return last - first;
    };
    nlohmann::json result{
        {"schema", 1},
        {"scope", "measurement-window-native-replay"},
        {"legacy_scope", "complete-combat-without-stage-not-local-movement-prediction"}};
    for (usize domain = 0; domain < OnlineCombatReplay::kDomains.size(); ++domain) {
        const auto& first = before.domains[domain];
        const auto& last = after.domains[domain];
        auto& output = result[OnlineCombatReplay::kDomains[domain]];
        output = {
            {"updates", difference(first.updates, last.updates)},
            {"passes", difference(first.passes, last.passes)},
            {"refusals", difference(first.refusals, last.refusals)},
            {"advanced_ticks", difference(first.advancedTicks, last.advancedTicks)},
            {"invalid_ticks", difference(first.invalidTicks, last.invalidTicks)},
            {"unsupported_ticks", difference(first.unsupportedTicks, last.unsupportedTicks)},
            {"presents", difference(first.presents, last.presents)},
            {"present_refusals", difference(first.presentRefusals, last.presentRefusals)},
            {"decode_attempts", difference(first.decodeAttempts, last.decodeAttempts)},
            {"decode_successes", difference(first.decodeSuccesses, last.decodeSuccesses)},
            {"decode_failures", difference(first.decodeFailures, last.decodeFailures)},
            {"decode_nanoseconds", difference(first.decodeNanoseconds, last.decodeNanoseconds)},
            {"advance_nanoseconds", difference(first.advanceNanoseconds, last.advanceNanoseconds)},
            {"restore_passes", difference(first.restorePasses, last.restorePasses)},
            {"append_passes", difference(first.appendPasses, last.appendPasses)},
            {"reused_passes", difference(first.reusedPasses, last.reusedPasses)},
            {"attempted_ticks", difference(first.attemptedTicks, last.attemptedTicks)},
            {"maximum_ticks_per_pass_since_begin", last.maximumTicksPerPass}};
        const auto timing = [&](std::string_view name, const OnlineCombatReplay::Timing& initial,
                                const OnlineCombatReplay::Timing& final) {
            output["cpu"][name] = {
                {"count", difference(initial.count, final.count)},
                {"nanoseconds", difference(initial.nanoseconds, final.nanoseconds)},
                {"maximum_nanoseconds_since_begin", final.maximumNanoseconds}};
        };
        timing("full_pass", first.passCost, last.passCost);
        timing("seed_copy_and_decode", first.seedCost, last.seedCost);
        timing("stage_decode", first.decodeCost, last.decodeCost);
        timing("restore", first.restoreCost, last.restoreCost);
        timing("native_advance", first.advanceCost, last.advanceCost);
        timing("tick_capture", first.captureCost, last.captureCost);
        timing("tick_prepare", first.prepareCost, last.prepareCost);
        timing("tick_postcondition", first.postconditionCost, last.postconditionCost);
        for (usize owner = 0; owner < OrdinaryCombatCheckpoint::kCaptureOwners.size(); ++owner) {
            timing("postcondition_" + std::string(OrdinaryCombatCheckpoint::kCaptureOwners[owner]),
                   first.postconditionOwners[owner], last.postconditionOwners[owner]);
        }
        timing("picture", first.pictureCost, last.pictureCost);
        for (usize reason = 0; reason < OnlineCombatReplay::kRefusals.size(); ++reason) {
            output["reasons"][OnlineCombatReplay::kRefusals[reason]] =
                difference(first.reasons[reason], last.reasons[reason]);
        }
        for (usize reason = 0; reason < OnlineCombatReplay::kPresentRefusals.size(); ++reason) {
            output["present_reasons"][OnlineCombatReplay::kPresentRefusals[reason]] =
                difference(first.presentReasons[reason], last.presentReasons[reason]);
        }
        // Numeric keys are the exact PlayerAnimator::Action enumerators, not a
        // second naming table which can silently drift from native behavior.
        for (usize action = 0; action < OrdinaryCombatTick::kActionCount; ++action) {
            output["refused_actions"][std::to_string(action)] =
                difference(first.refusedActions[action], last.refusedActions[action]);
        }
        for (usize target = 0; target < OrdinaryCombatTick::kTargetFamilies.size(); ++target) {
            output["refused_targets"][OrdinaryCombatTick::kTargetFamilies[target]] =
                difference(first.refusedTargets[target], last.refusedTargets[target]);
        }
        for (usize issue = 0; issue < OrdinaryCombatCheckpoint::kRestoreIssues.size(); ++issue) {
            output["seed_issues"][OrdinaryCombatCheckpoint::kRestoreIssues[issue]] =
                difference(first.seedIssues[issue], last.seedIssues[issue]);
        }
        for (usize phase = 0; phase < OrdinaryCombatTick::kRefusals.size(); ++phase) {
            output["native_refusals"][OrdinaryCombatTick::kRefusals[phase]] =
                difference(first.tickRefusals[phase], last.tickRefusals[phase]);
        }
        for (usize reason = 0; reason < OrdinaryCombatTick::kPartyRefusals.size(); ++reason) {
            output["party_refusals"][OrdinaryCombatTick::kPartyRefusals[reason]] =
                difference(first.partyRefusals[reason], last.partyRefusals[reason]);
        }
    }
    result["counter_resets"] = resets;
    return result;
}

void run(const Options& options, Clock::time_point processBegin) {
    const GameConfig config;
    LevelCatalog levels;
    StringTable strings;
    require(levels.load(options.assets), "Native assets missing");
    require(strings.load(options.data / "text", "en"), "Shipped text missing");
    const auto requestedLevel =
        options.stage == "tower" ? std::optional{LevelRef::tower()} : levels.byName(options.stage);
    require(requestedLevel.has_value(), "Requested native stage is missing");
    const auto requestedScene = OnlinePlay::sceneId(levels, *requestedLevel);
    require(requestedScene.has_value(), "Requested stage has no native scene ID");
    if (options.requireVolleys) {
        ClassDataSet classes;
        require(classes.load(options.assets / "PDATA"), "Native class data missing");
        const auto* stats = classes.stats(options.character);
        require(stats != nullptr, "Requested native class data missing");
        auto rows = stats->strikesOf(options.workload == "turbo-C" ? stats->moves.turboC1
                                                                   : stats->moves.turboB);
        if (options.workload == "turbo-C") {
            const auto second = stats->strikesOf(stats->moves.turboC2);
            rows.insert(rows.end(), second.begin(), second.end());
        }
        require(std::ranges::any_of(rows,
                                    [&](s32 row) {
                                        return stats->moveStrikes[static_cast<usize>(row)].type ==
                                               MoveStrike::kVolley;
                                    }),
                "Selected native turbo has no volley; choose a volley class such as Archer (3)");
    }
    const auto content = assetDigest(options.assets);
    GameContext context;
    context.config = &config;
    context.levels = &levels;
    context.strings = &strings;
    context.unpackedRoot = options.assets;
    std::string error;
    auto wire = openSessionTransport(
        {!options.internet, options.host ? std::string{} : readInvitation(options.invitation)},
        error);
    require(wire != nullptr, "Could not open iroh transport");
    // Refuse separate-machine peers that provision a different native workload.
    // Presentation rate and prediction intentionally remain local A/B settings.
    const std::string probeBuild = "native-latency-probe-4:" + options.stage + ":" +
                                   options.workload + ":" + options.element + ":" +
                                   std::to_string(options.character) + ":" +
                                   (options.requireVolleys ? "volleys" : "startup");
    test::MeasuredPacketTransport measuredWire(*wire);
    HostedRoom room(measuredWire, options.host, 1, probeBuild, content);
    OnlineSession session(room, room.transport(), options.host, 1);
    OnlineParty party;
    PartyMember member;
    member.player = 0;
    member.save.name = "PROBE";
    member.save.color = options.host ? 0 : 1;
    member.save.character = options.character;
    member.save.progress().health = 1000;
    member.save.progress().promotedLevel = -1;
    member.save.moviesSeen = {"PRIVATE"};
    require(party.select(session, std::array{member}), "Could not select probe character");
    test::FakeRenderDevice device;
    device.recordDraws = false;
    OnlinePlay play;
    play.setMovementPrediction(options.prediction);
    bool invited = false;
    bool opened = false;
    bool reported = false;
    std::optional<Clock::time_point> started;
    const auto launch = Clock::now();
    auto nextUpdate = launch;
    auto nextRender = launch;
    auto lastRender = launch;
    const auto tickTime =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<f64>(1.0 / 60));
    const auto frameTime =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<f64>(1.0 / options.fps));
    LatencyTelemetry::Distribution updateCost;
    LatencyTelemetry::Distribution renderCost;
    LoopDiagnostics diagnostics;
    StageDiagnostics stage;
    OnlineCombatReplay::Diagnostics replayBegin;
    std::optional<PlayScene::CombatCaptureCosts> captureBegin;
    u64 overruns = 0;
    s32 lastThrow = -1;
    const bool speedWorkload = options.workload == "speed-rapid-five";
    const bool itemWorkload = options.workload == "rapid-five" || speedWorkload;
    const u32 element = elementFlag(options.element);
    const bool turboWorkload = options.workload != "throw" && !itemWorkload;
    const bool fullTurbo = options.workload == "turbo-C";
    const bool shoveWorkload = options.workload == "shove";
    const auto turboAction = shoveWorkload ? PlayerAnimator::Action::Shove
                             : fullTurbo   ? PlayerAnimator::Action::TurboFull
                                           : PlayerAnimator::Action::TurboStrong;
    TurboDiagnostics turbo;
    ThrowDiagnostics throws;
    FanDiagnostics fan;
    ElementDiagnostics elemental;
    VolleyDiagnostics volleys;
    u64 turboFunding = 0;
    struct ThrowProbe {
        Clock::time_point pressed;
        CombatSnapshot shown;
        ThrowDiagnostics::Observation hostSeed;
        u64 updates = 0;
    };
    std::optional<ThrowProbe> pendingThrow;
    while (Clock::now() - launch < std::chrono::seconds(180 + options.seconds)) {
        auto now = Clock::now();
        if (options.host && !invited && wire->phase() == SessionTransport::Phase::Ready) {
            writeFile(options.invitation, wire->invitation());
            invited = true;
        }
        if (now >= nextUpdate) {
            if (opened) {
                SessionInputs::Frame input;
                if (turboWorkload && options.host) {
                    turboFunding += fundTurbo(play, fullTurbo);
                }
                if ((itemWorkload || element != 0) && options.host) {
                    equipItems(play, itemWorkload, speedWorkload, element);
                }
                if (started && !options.host) {
                    const f64 elapsed = std::chrono::duration<f64>(now - *started).count();
                    if (elapsed >= 3 && elapsed < options.seconds) {
                        const auto pulse = static_cast<s32>((elapsed - 3) / 1.5);
                        const f64 phase = std::fmod(elapsed - 3, 1.5);
                        if (phase < 0.25) {
                            input[0].move = {{pulse % 2 == 0 ? 1.0f : -1.0f, 0}, 1};
                        }
                        // Exercise an authoritative attack between movement pulses.
                        // A one-tick press must produce a rendered throw, not merely
                        // an acknowledged command or a predicted walking animation.
                        if (!turboWorkload && phase >= 0.6 && lastThrow != pulse &&
                            elapsed < options.seconds - 1.1 && !pendingThrow &&
                            test::LatencyProbeEligibility::idleForThrow(
                                play.shown(), session.match().playback().latest(),
                                session.match().context().epoch, session.seats().front(),
                                session.match().context().grants[session.seats().front()])) {
                            lastThrow = pulse;
                            input[0].attack = input[0].attackPressed = true;
                            pendingThrow = ThrowProbe{
                                now, *play.shown(),
                                ThrowDiagnostics::observation(*session.match().playback().latest(),
                                                              session.seats().front(), true)};
                        }
                        if (turboWorkload && phase >= 0.6 && lastThrow != pulse &&
                            elapsed < options.seconds - 2.1 &&
                            test::LatencyProbeEligibility::ready(
                                play.shown(), session.match().playback().latest(),
                                session.match().context().epoch, session.seats().front(),
                                session.match().context().grants[session.seats().front()]) &&
                            turbo.arm(*play.shown(), *session.match().playback().latest(),
                                      session.seats().front(), turboAction, now)) {
                            lastThrow = pulse;
                            input[0].turboAttackPressed = !shoveWorkload;
                            input[0].chargePressed = shoveWorkload;
                        }
                    }
                }
                const auto playBegin = Clock::now();
                const auto phase = play.update(input);
                diagnostics.record(LoopDiagnostics::Operation::PlayUpdate, playBegin, Clock::now(),
                                   processBegin, started, phase);
                if (phase == OnlinePlay::Phase::Failed) {
                    if (reported) {
                        return; // A peer finished its report and disconnected.
                    }
                    throw std::runtime_error("Native gameplay failed");
                }
                if (started && !reported) {
                    require(session.match().context().scene == *requestedScene,
                            "Probe traveled away from its requested stage");
                    stage.observe(options.host ? play.shown()
                                               : session.match().playback().latest());
                }
                // Adaptive lead and backpressure can change the accepted tick
                // during update. Associate the original press with its actual
                // sampled command, never a guessed tick or a receipt watermark.
                if (pendingThrow) {
                    ++pendingThrow->updates;
                    for (const auto& command : session.match().sampledInputs()) {
                        if (command.seat == session.seats().front() &&
                            command.pressed(CommandPress::Attack)) {
                            session.match().latency().throwRequest(command, pendingThrow->shown,
                                                                   pendingThrow->pressed);
                            throws.bind(command, pendingThrow->shown, pendingThrow->hostSeed,
                                        pendingThrow->pressed, pendingThrow->updates);
                            pendingThrow.reset();
                            break;
                        }
                    }
                }
                if (!turboWorkload && started && !reported) {
                    if (options.host) {
                        throws.observePhysical(play.hostScene(), play.shown());
                    } else {
                        throws.observe(session.match().playback().latest(), Clock::now());
                    }
                }
                if (turboWorkload && !options.host) {
                    turbo.bind(session.match().sampledInputs());
                    turbo.observe(session.match().playback().latest(), true, Clock::now());
                    if (options.requireVolleys) {
                        volleys.observe(session.match().playback().latest(),
                                        session.seats().front(), true);
                    }
                }
                if (itemWorkload && !options.host) {
                    fan.observe(session.match().playback().latest(), session.seats().front(), true,
                                element);
                }
                if (element != 0 && !options.host) {
                    elemental.observe(session.match().playback().latest(), session.seats().front(),
                                      true, element);
                }
            } else {
                const auto sessionBegin = Clock::now();
                session.update(1.0 / 60);
                diagnostics.record(LoopDiagnostics::Operation::SessionUpdate, sessionBegin,
                                   Clock::now(), processBegin, started, play.phase());
                if (session.phase() == OnlineSession::Phase::Lobby && session.connected()) {
                    session.ready();
                    const auto* state = session.room();
                    if (options.host && state != nullptr &&
                        state->members.size() == static_cast<usize>(options.players)) {
                        session.start();
                    }
                }
                if (session.phase() == OnlineSession::Phase::Active) {
                    PlayOptions settings;
                    settings.welcome = false;
                    if (requestedLevel->isTower()) {
                        settings.position = Vec3{0};
                    }
                    const auto sceneBegin = Clock::now();
                    const bool loaded =
                        play.open(device, context, session, party, *requestedLevel, settings);
                    diagnostics.record(LoopDiagnostics::Operation::SceneOpen, sceneBegin,
                                       Clock::now(), processBegin, started, play.phase());
                    require(loaded, "Could not load native scene");
                    opened = true;
                }
            }
            require(session.phase() != OnlineSession::Phase::Failed, "Online session failed");
            const auto updateEnd = Clock::now();
            updateCost.add(std::chrono::duration<f64, std::milli>(updateEnd - now).count());
            diagnostics.record(LoopDiagnostics::Operation::Update, now, updateEnd, processBegin,
                               started, play.phase());
            nextUpdate += tickTime;
            if (Clock::now() - nextUpdate > std::chrono::milliseconds(100)) {
                ++overruns;
                nextUpdate = Clock::now(); // report overload, never conceal it with a burst
            }
        }
        now = Clock::now();
        if (opened && now >= nextRender) {
            device.beginFrame();
            const f64 delta = std::chrono::duration<f64>(now - lastRender).count();
            const f32 blend = std::clamp(
                static_cast<f32>(1 - std::chrono::duration<f64>(nextUpdate - now).count() * 60),
                0.0f, 1.0f);
            play.render(device, Mat4{1}, 1280, 720, delta, blend);
            device.endFrame();
            lastRender = now;
            nextRender += frameTime;
            if (nextRender < now) {
                nextRender = now + frameTime;
            }
            const auto renderEnd = Clock::now();
            if (!turboWorkload && !options.host && started && !reported) {
                throws.presented(play.shown(), renderEnd);
            }
            if (turboWorkload && !options.host) {
                turbo.observe(play.shown(), false, renderEnd);
                if (options.requireVolleys) {
                    volleys.observe(play.shown(), session.seats().front(), false);
                }
            }
            if (itemWorkload && !options.host) {
                fan.observe(play.shown(), session.seats().front(), false, element);
            }
            if (element != 0 && !options.host) {
                elemental.observe(play.shown(), session.seats().front(), false, element);
            }
            renderCost.add(std::chrono::duration<f64, std::milli>(renderEnd - now).count());
            diagnostics.record(LoopDiagnostics::Operation::Render, now, renderEnd, processBegin,
                               started, play.phase());
            if (!started && play.phase() == OnlinePlay::Phase::Playing && play.shown()) {
                const auto seat = session.seats().front();
                const auto* authority =
                    options.host ? play.shown() : session.match().playback().latest();
                if (test::LatencyProbeEligibility::ready(play.shown(), authority,
                                                         session.match().context().epoch, seat,
                                                         session.match().context().grants[seat])) {
                    require(session.match().context().scene == *requestedScene &&
                                OnlinePlay::levelOf(levels, session.match().context().scene) ==
                                    requestedLevel,
                            "Loaded session stage differs from requested stage");
                    if (options.host) {
                        require(play.hostScene() != nullptr &&
                                    play.hostScene()->world() != nullptr &&
                                    play.hostScene()->world()->ref() == *requestedLevel,
                                "Host native world differs from requested stage");
                    } else {
                        require(play.replica() != nullptr,
                                "Guest native stage resources have not loaded");
                    }
                    started = Clock::now();
                    measuredWire.reset();
                    replayBegin = play.combatReplayDiagnostics();
                    if (options.host && play.hostScene() != nullptr) {
                        captureBegin = play.hostScene()->combatCaptureCosts();
                    }
                }
            }
        }
        if (!reported && started &&
            Clock::now() - *started >= std::chrono::seconds(options.seconds)) {
            const auto reportAt = Clock::now();
            auto report = nlohmann::json::parse(session.match().latency().report(true));
            report["unadmitted_attack_requests"] = pendingThrow ? 1 : 0;
            report["unadmitted_attack_request"] = nullptr;
            if (pendingThrow) {
                const auto* latest = session.match().playback().latest();
                report["unadmitted_attack_request"] = {
                    {"request_monotonic_ms", std::chrono::duration<f64, std::milli>(
                                                 pendingThrow->pressed.time_since_epoch())
                                                 .count()},
                    {"age_ms_at_report",
                     std::chrono::duration<f64, std::milli>(reportAt - pendingThrow->pressed)
                         .count()},
                    {"native_updates_waited", pendingThrow->updates},
                    {"input_ready_at_report", session.match().inputReady()},
                    {"next_input_tick_at_report", session.match().inputTick()},
                    {"authority_tick_at_report", latest != nullptr
                                                     ? nlohmann::json(latest->motion.tick)
                                                     : nlohmann::json(nullptr)},
                    {"shown_seed", ThrowDiagnostics::encode(ThrowDiagnostics::observation(
                                       pendingThrow->shown, session.seats().front(), false))},
                    {"host_seed", ThrowDiagnostics::encode(pendingThrow->hostSeed)}};
            }
            report["role"] = options.host ? "host" : "guest";
            report["prediction"] = options.prediction;
            report["workload"] = options.workload;
            report["element"] = options.element;
            report["character"] = options.character;
            report["require_volleys"] = options.requireVolleys;
            const auto loadedLevel = OnlinePlay::levelOf(levels, session.match().context().scene);
            require(loadedLevel == requestedLevel, "Report stage differs from requested stage");
            report["stage"] = options.stage;
            report["loaded_stage"] = loadedLevel->isTower() ? "tower" : loadedLevel->name;
            report["scene_id"] = session.match().context().scene;
            stage.append(report, options.host);
            if (captureBegin && play.hostScene() != nullptr) {
                const auto& last = play.hostScene()->combatCaptureCosts();
                const auto cost = [](const auto& first, const auto& final) {
                    require(final.count >= first.count && final.nanoseconds >= first.nanoseconds,
                            "Host capture counters reset during measurement");
                    return nlohmann::json{
                        {"count", final.count - first.count},
                        {"nanoseconds", final.nanoseconds - first.nanoseconds},
                        {"maximum_nanoseconds_since_scene_creation", final.maximumNanoseconds}};
                };
                require(last.accepted >= captureBegin->accepted,
                        "Host accepted capture count reset during measurement");
                report["host_capture_cpu"] = {
                    {"scope", "measurement-window-existing-calls-including-refusals"},
                    {"accepted", last.accepted - captureBegin->accepted},
                    {"total", cost(captureBegin->total, last.total)},
                    {"capture", cost(captureBegin->capture, last.capture)},
                    {"stage_encode", cost(captureBegin->encode, last.encode)}};
                report["combat_seeds"]["capture_nanoseconds"] =
                    last.capture.nanoseconds - captureBegin->capture.nanoseconds;
                report["combat_seeds"]["encode_nanoseconds"] =
                    last.encode.nanoseconds - captureBegin->encode.nanoseconds;
                report["combat_seeds"]["timing_scope"] =
                    "measurement-window-existing-host-calls-including-refusals";
            }
            report["combat_replay"] = replayReport(replayBegin, play.combatReplayDiagnostics());
            if (!turboWorkload) {
                throws.append(report);
            }
            if (options.requireVolleys) {
                volleys.append(report);
            }
            if (element != 0) {
                elemental.append(report, element);
            }
            if (itemWorkload) {
                fan.append(report, speedWorkload);
            }
            if (turboWorkload) {
                turbo.append(report);
                report["turbo_funding_count"] = turboFunding;
                report["turbo_host_funding_enabled"] = true;
            }
            report["render_fps"] = options.fps;
            report["headless"] = true;
            report["timing_scope"] = "visual-submit-no-audio";
            report["internet_enabled"] = options.internet;
            report["update_p95_ms"] = updateCost.summary().p95;
            report["render_p95_ms"] = renderCost.summary().p95;
            const auto& traffic = measuredWire.counters();
            const f64 networkSeconds = std::chrono::duration<f64>(Clock::now() - *started).count();
            report["network_payload_schema"] = 1;
            report["network_payload_scope"] = "gameplay-provider-payload-no-protocol-overhead";
            report["network_measurement_seconds"] = networkSeconds;
            report["network_sent_bytes"] = traffic.sentBytes;
            report["network_received_bytes"] = traffic.receivedBytes;
            report["network_sent_packets"] = traffic.sentPackets;
            report["network_received_packets"] = traffic.receivedPackets;
            report["network_sent_bytes_per_second"] = traffic.sentBytes / networkSeconds;
            report["network_received_bytes_per_second"] = traffic.receivedBytes / networkSeconds;
            report["network_unreliable_sent_bytes"] = traffic.deliveryBytes[0];
            report["network_reliable_sent_bytes"] = traffic.deliveryBytes[1];
            report["network_send_results"] = traffic.sendResults;
            diagnostics.append(report);
            report["process_begin_monotonic_ms"] =
                std::chrono::duration<f64, std::milli>(processBegin.time_since_epoch()).count();
            report["gameplay_begin_monotonic_ms"] =
                std::chrono::duration<f64, std::milli>(started->time_since_epoch()).count();
            report["overruns"] = overruns;
            report["input_lead_ticks"] = session.match().inputLead();
            report["corrections"] = play.predictionMetrics().corrections;
            report["largest_correction"] = play.predictionMetrics().largestCorrection;
            report["status"] = pendingThrow ? "failed" : "complete";
            writeFile(options.report, report.dump(2));
            std::cout << "Latency report: " << options.report.string() << '\n';
            // Retain real CPU/network evidence on failure, without accepting an
            // unadmitted request or weakening the probe's nonzero exit status.
            require(!pendingThrow, "Probe attack was never admitted to a local input tick");
            reported = true;
            if (!options.host && !options.holdAfterReport) {
                return;
            }
        }
        if (reported && started &&
            Clock::now() - *started >= std::chrono::seconds(options.seconds + 10)) {
            return;
        }
        const auto wait = std::min(nextUpdate, opened ? nextRender : nextUpdate) - Clock::now();
        if (wait > Clock::duration::zero()) {
            SDL_DelayPrecise(static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(wait).count()));
        }
    }
    throw std::runtime_error("Probe timed out before completing measurements");
}
} // namespace
int main(int argc, char** argv) {
    const auto processBegin = Clock::now();
    try {
        run(parse(std::span(argv, static_cast<usize>(argc))), processBegin);
        return 0;
    } catch (const std::exception& error) {
        // Error reporting must not allocate or let another exception escape main.
        std::fputs("Latency probe: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return 1;
    } catch (...) {
        return 1;
    }
}
