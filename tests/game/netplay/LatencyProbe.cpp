// Native, independently paced host/guest processes. The renderer runs the real
// presentation code but discards GPU commands. Audio is not initialized: these
// timings measure visual submission, not sound output or monitor scanout.
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <set>

#include <SDL3/SDL_timer.h>
#include <nlohmann/json.hpp>

#include "engine/core/Types.h"
#include "engine/net/SessionTransport.h"

#include "FakeRenderDevice.h"
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
bool idleForThrow(const CombatSnapshot* state, u8 seat, bool native) {
    if (state == nullptr || seat >= state->players.size() || !state->players[seat]) {
        return false;
    }
    const auto& player = *state->players[seat];
    const auto action = native && player.rollback ? static_cast<u32>(player.rollback->action)
                                                  : player.animation.action;
    return player.movement.enabled && action <= static_cast<u32>(PlayerAnimator::Action::Idle2Loop);
}

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
        if (!idleForThrow(state, seat, native) || !state->motion.players[seat]) {
            return false;
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

void run(const Options& options, Clock::time_point processBegin) {
    const GameConfig config;
    LevelCatalog levels;
    StringTable strings;
    require(levels.load(options.assets), "Native assets missing");
    require(strings.load(options.data / "text", "en"), "Shipped text missing");
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
    const std::string probeBuild = "native-latency-probe-3:" + options.workload + ":" +
                                   options.element + ":" + std::to_string(options.character) + ":" +
                                   (options.requireVolleys ? "volleys" : "startup");
    HostedRoom room(*wire, options.host, 1, probeBuild, content);
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
    FanDiagnostics fan;
    ElementDiagnostics elemental;
    VolleyDiagnostics volleys;
    u64 turboFunding = 0;
    struct ThrowProbe {
        Clock::time_point pressed;
        CombatSnapshot shown;
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
                            idleForThrow(play.shown(), session.seats().front(), false) &&
                            idleForThrow(session.match().playback().latest(),
                                         session.seats().front(), true)) {
                            lastThrow = pulse;
                            input[0].attack = input[0].attackPressed = true;
                            pendingThrow = ThrowProbe{now, *play.shown()};
                        }
                        if (turboWorkload && phase >= 0.6 && lastThrow != pulse &&
                            elapsed < options.seconds - 2.1 && play.shown() != nullptr &&
                            session.match().playback().latest() != nullptr &&
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
                // Adaptive lead and backpressure can change the accepted tick
                // during update. Associate the original press with its actual
                // sampled command, never a guessed tick or a receipt watermark.
                if (pendingThrow) {
                    for (const auto& command : session.match().sampledInputs()) {
                        if (command.seat == session.seats().front() &&
                            command.pressed(CommandPress::Attack)) {
                            session.match().latency().throwRequest(command, pendingThrow->shown,
                                                                   pendingThrow->pressed);
                            pendingThrow.reset();
                            break;
                        }
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
                    settings.position = Vec3{0};
                    const auto sceneBegin = Clock::now();
                    const bool loaded =
                        play.open(device, context, session, party, LevelRef::tower(), settings);
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
                const auto& player = play.shown()->players[seat];
                if (player && player->movement.enabled) {
                    started = Clock::now();
                }
            }
        }
        if (!reported && started &&
            Clock::now() - *started >= std::chrono::seconds(options.seconds)) {
            require(!pendingThrow, "Probe attack was never admitted to a local input tick");
            auto report = nlohmann::json::parse(session.match().latency().report(true));
            report["role"] = options.host ? "host" : "guest";
            report["prediction"] = options.prediction;
            report["workload"] = options.workload;
            report["element"] = options.element;
            report["character"] = options.character;
            report["require_volleys"] = options.requireVolleys;
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
            diagnostics.append(report);
            report["process_begin_monotonic_ms"] =
                std::chrono::duration<f64, std::milli>(processBegin.time_since_epoch()).count();
            report["gameplay_begin_monotonic_ms"] =
                std::chrono::duration<f64, std::milli>(started->time_since_epoch()).count();
            report["overruns"] = overruns;
            report["input_lead_ticks"] = session.match().inputLead();
            report["corrections"] = play.predictionMetrics().corrections;
            report["largest_correction"] = play.predictionMetrics().largestCorrection;
            report["status"] = "complete";
            writeFile(options.report, report.dump(2));
            std::cout << "Latency report: " << options.report.string() << '\n';
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
