// Runs the real gameplay update in an independent process. There is no network
// authority, replica, replay admission gate, or OrdinaryCombatTick in this probe.
// The observation explicitly reports incomplete coverage: equal traces are not
// proof that the entire scene can already be restored or replayed deterministically.
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/assets/StringTable.h"
#include "engine/io/Sha256.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "NativeProbeLink.h"
#include "NativeProbeTiming.h"
#include "game/app/NativeOnlineIdentity.h"
#include "game/netplay/AssetDigest.h"
#include "game/netplay/InputCommand.h"
#include "game/screens/SessionInputs.h"
#include "game/screens/SimulationObservation.h"
#include "game/screens/SimulationObserver.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Json = nlohmann::json;

struct Options {
    std::filesystem::path report;
    std::filesystem::path timings;
    u32 timingWarmup = 0;
    std::filesystem::path assets = GDL_TEST_ASSET_DIR;
    std::filesystem::path data = GDL_TEST_DATA_DIR;
    std::string stage = "G1";
    std::string workload = "combat";
    u32 ticks = 600;
    u32 players = 2;
    u32 renderEvery = 0;
    u32 perturbTick = 0;
    std::string networkRole;
    u32 networkPeer = 0;
    std::filesystem::path ticket;
    u32 inputDelay = 0;
};

void require(bool condition, std::string_view reason) {
    if (!condition) {
        throw std::runtime_error(std::string(reason));
    }
}

u32 number(std::string_view value) {
    u32 result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(),
            "Expected an unsigned integer");
    return result;
}

Options parse(std::span<char*> args) {
    Options result;
    for (usize index = 1; index < args.size(); ++index) {
        const std::string_view key = args[index];
        require(index + 1 < args.size(), "Missing option value");
        const std::string_view value = args[++index];
        if (key == "--report") {
            result.report = value;
        } else if (key == "--timings") {
            result.timings = value;
        } else if (key == "--timing-warmup") {
            result.timingWarmup = number(value);
        } else if (key == "--assets") {
            result.assets = value;
        } else if (key == "--data") {
            result.data = value;
        } else if (key == "--stage") {
            result.stage = value;
        } else if (key == "--workload") {
            require(value == "idle" || value == "combat", "Workload must be idle or combat");
            result.workload = value;
        } else if (key == "--ticks") {
            result.ticks = number(value);
        } else if (key == "--players") {
            result.players = number(value);
        } else if (key == "--render-every") {
            result.renderEvery = number(value);
        } else if (key == "--perturb-tick") {
            result.perturbTick = number(value);
        } else if (key == "--network-role") {
            require(value == "host" || value == "guest", "Network role must be host or guest");
            result.networkRole = value;
        } else if (key == "--ticket") {
            result.ticket = value;
        } else if (key == "--network-peer") {
            result.networkPeer = number(value);
        } else if (key == "--input-delay") {
            result.inputDelay = number(value);
        } else {
            throw std::runtime_error("Unknown option: " + std::string(key));
        }
    }
    require(!result.report.empty(), "--report is required");
    require(!std::filesystem::exists(result.report), "Report exists; choose a new path");
    require(result.ticks > 0 && result.ticks <= 36'000, "Ticks must be 1..36000");
    require(!result.timings.empty() || result.timingWarmup == 0,
            "Timing warmup requires --timings");
    require(result.timingWarmup < result.ticks, "Timing warmup must leave measured ticks");
    if (!result.timings.empty()) {
        require(!std::filesystem::exists(result.timings), "Timings exist; choose a new path");
        require(std::filesystem::weakly_canonical(result.timings) !=
                    std::filesystem::weakly_canonical(result.report),
                "Timing and observation reports must be distinct");
    }
    require(result.players >= 1 && result.players <= 4, "Players must be 1..4");
    require(result.renderEvery <= 60, "Render interval must be 0..60 ticks");
    require(result.perturbTick <= result.ticks, "Perturbation is outside the run");
    require(result.inputDelay <= LockstepInputs::kMaxDelay, "Input delay must be 0..6");
    require(result.networkRole.empty() == result.ticket.empty(), "Network role requires a ticket");
    require(!result.networkRole.empty() || result.inputDelay == 0, "Delay requires network mode");
    require(result.networkRole.empty() || result.players >= 2,
            "Network probe requires two to four players");
    if (!result.networkRole.empty()) {
        if (result.networkPeer == 0) {
            result.networkPeer = result.networkRole == "host" ? 1 : 2;
        }
        require(
            result.networkPeer <= result.players &&
                (result.networkRole == "host" ? result.networkPeer == 1 : result.networkPeer >= 2),
            "Network peer does not match its role or player count");
    } else {
        require(result.networkPeer == 0, "Network peer requires network mode");
    }
    return result;
}

std::string hex(std::span<const u8> bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const u8 byte : bytes) {
        result += digits[byte >> 4U];
        result += digits[byte & 15U];
    }
    return result;
}

std::string digest(std::span<const u8> bytes) {
    Sha256 hash;
    hash.update(bytes);
    return hash.hex();
}

// Journal v1 is tick-indexed, including startup. Aim is already resolved into
// world space, just as it must be at the device/network input boundary. No mouse
// projection, current camera, wall clock or render cadence selects these inputs.
std::vector<InputCommand> journal(const Options& options, u32 tick) {
    constexpr std::array<Vec2, 4> directions{Vec2{0, 1}, Vec2{1, 0}, Vec2{0, -1}, Vec2{-1, 0}};
    std::vector<InputCommand> commands;
    for (u32 seat = 0; seat < options.players; ++seat) {
        PlayInput input;
        if (options.workload == "combat" && tick > 360) {
            const u32 elapsed = tick - 361;
            const u32 phase = (elapsed + seat * 30) % 240;
            const Vec2 direction = directions[(elapsed / 120 + seat) % directions.size()];
            input.move = {direction, 1};
            input.aimDirection = Vec3{direction.x, 0, -direction.y};
            input.attack = phase < 90;
            input.attackPressed = phase == 0;
            input.strongAttack = phase >= 120 && phase < 150;
            input.turbo = phase >= 180 && phase < 190;
            input.defendPressed = phase == 180;
            input.usePotion = elapsed % 600 == 300;
            input.menu.select = elapsed % 120 == 0;
            input.menu.buttonPressed = input.menu.select;
        }
        // Negative control changes the resolved journal, not scene internals.
        if (seat == 0 && tick == options.perturbTick) {
            input.move = {{-1, 0}, 1};
            input.aimDirection = Vec3{-1, 0, 0};
            input.usePotion = true;
        }
        commands.push_back(SessionInputs::command(input, 1, tick, 1, static_cast<u8>(seat)));
    }
    return commands;
}

void run(const Options& options) {
    GameConfig config;
    config.timing.tickRate = 60;
    LevelCatalog levels;
    StringTable strings;
    require(levels.load(options.assets), "Native level catalog missing");
    require(strings.load(options.data / "text", "en"), "Shipped text missing");
    const auto level =
        options.stage == "tower" ? std::optional{LevelRef::tower()} : levels.byName(options.stage);
    require(level.has_value(), "Requested stage missing from native catalog");
    const std::string assetsDigest = assetDigest(options.assets);
    const std::string dataDigest = assetDigest(options.data);
    const std::string configuration = config.toJson();
    const auto configBytes =
        std::span{reinterpret_cast<const u8*>(configuration.data()), configuration.size()};
    test::FakeRenderDevice device;
    device.recordDraws = false;
    LevelWorld world;
    require(world.load(device, options.assets, *level), "Could not load native stage");
    GameContext context;
    context.config = &config;
    context.levels = &levels;
    context.strings = &strings;
    context.unpackedRoot = options.assets;
    f32 stopTimeTotal = 0;
    context.stopTimeTotal = &stopTimeTotal;
    // No save slots, persisted settings, audio devices or other external side
    // effects. Their exclusion is part of the observation's coverage contract.
    std::vector<PartyMember> party;
    for (u32 seat = 0; seat < options.players; ++seat) {
        CharacterSave character;
        character.name = "PROBE";
        character.character = static_cast<s32>(seat);
        character.color = static_cast<s32>(seat);
        character.progress().health = 2000;
        character.progress().inventory.addKeys(9);
        character.progress().inventory.addPotions(static_cast<s32>(seat + 1), 9);
        party.push_back({static_cast<s32>(seat), character});
    }
    PlayScene scene;
    PlayOptions openOptions;
    openOptions.welcome = false;
    require(scene.open(device, context, world, party, openOptions), "Could not open native scene");
    // Existing catalogs resolve immutable artwork identities for diagnostic
    // hashing only. They do not select or run the old guest simulation.
    SimulationObserver observer{scene};
    require(observer.bind(), "Could not bind native observation resources");

    std::ofstream report(options.report, std::ios::binary);
    require(report.is_open(), "Could not open report");
    const auto write = [&](const Json& row) {
        report << row.dump() << '\n';
        require(report.good(), "Could not write report");
    };
    const std::string configDigest = digest(configBytes);
    std::unique_ptr<test::NativeProbeLink> network;
    if (!options.networkRole.empty()) {
        const std::string contract = Json{{"assets", assetsDigest},
                                          {"data", dataDigest},
                                          {"config", configDigest},
                                          {"stage", options.stage},
                                          {"ticks", options.ticks},
                                          {"workload", options.workload},
                                          {"delay", options.inputDelay},
                                          {"players", options.players},
                                          {"protocol", "native-input-probe-v2"}}
                                         .dump();
        const auto contractBytes =
            std::span{reinterpret_cast<const u8*>(contract.data()), contract.size()};
        network = std::make_unique<test::NativeProbeLink>(
            options.networkRole == "host", options.ticket, digest(contractBytes),
            static_cast<u8>(options.inputDelay), static_cast<u8>(options.players),
            static_cast<u8>(options.networkPeer));
    }
    const Mat4 projection = glm::ortho(0.0f, 640.0f, 448.0f, 0.0f, -1.0f, 1.0f);
    u32 readyTicks = 0;
    u32 movedTicks = 0;
    usize mostEnemies = 0;
    usize mostProjectiles = 0;
    std::vector<f64> updateTimes, observationTimes, renderTimes;
    if (!options.timings.empty()) {
        updateTimes.reserve(options.ticks);
        observationTimes.reserve(options.ticks);
        renderTimes.reserve(options.ticks);
    }
    for (u32 tick = 1; tick <= options.ticks; ++tick) {
        const auto measure = [&](std::vector<f64>& samples, const auto& operation) {
            if (options.timings.empty() || tick <= options.timingWarmup) {
                operation();
                return;
            }
            const auto started = std::chrono::steady_clock::now();
            operation();
            const f64 elapsed = std::chrono::duration<f64, std::milli>(
                                    std::chrono::steady_clock::now() - started).count();
            samples.push_back(elapsed);
        };
        const auto commands = network ? network->frame([&](u64 inputTick, u8 seat) {
            return journal(options, static_cast<u32>(inputTick))[seat];
        })
                                      : journal(options, tick);
        const auto bytes = InputPacket::encode(commands);
        require(bytes.has_value(), "Invalid journal input");
        PlayScene::Inputs inputs;
        for (const auto& command : commands) {
            inputs[command.seat] = SessionInputs::playInput(command);
        }
        const Vec3 before = scene.actor(0)->position();
        PlayOutcome outcome = PlayOutcome::Running;
        measure(updateTimes, [&] { outcome = scene.update(1.0 / 60, inputs); });
        require(outcome == PlayOutcome::Running,
                "Scene requested travel or ended; campaign transitions are not exercised yet");
        if (network) {
            network->frameCompleted(tick % MatchSession::kNativeAgreementPeriod == 0
                                        ? scene.nativeAgreementDigest()
                                        : std::nullopt);
        }
        readyTicks += scene.spawning() ? 0U : 1U;
        movedTicks += before == scene.actor(0)->position() ? 0U : 1U;
        mostEnemies = std::max(mostEnemies, scene.enemies().count());
        mostProjectiles = std::max(mostProjectiles, scene.missiles().count());
        if (options.renderEvery != 0 && tick % options.renderEvery == 0) {
            measure(renderTimes, [&] {
                require(device.beginFrame(), "Headless frame refused");
                scene.render(device, projection, 640, 448, false, 0.5f);
                device.endFrame();
            });
        }
        SimulationObservation observation;
        measure(observationTimes, [&] { observation = observer.observe(); });
        std::ranges::sort(observation.subsystems, {}, &SimulationObservation::Subsystem::name);
        if (tick == 1) {
            std::vector<std::string> names;
            for (const auto& subsystem : observation.subsystems) {
                names.push_back(subsystem.name);
            }
            write({{"type", "metadata"},
                   {"schema", 1},
                   {"simulation", "PlayScene::update"},
                   {"stage", options.stage},
                   {"ticks", options.ticks},
                   {"players", options.players},
                   {"tick_rate", 60},
                   {"render_every", options.renderEvery},
                   {"workload", options.workload},
                   {"input_version", 1},
                   {"input_delivery", network ? "iroh-lockstep" : "local"},
                   {"input_delay", options.inputDelay},
                   {"initialization", "native-defaults-v1"},
                   {"assets_digest", assetsDigest},
                   {"data_digest", dataDigest},
                   {"config_digest", configDigest},
                   {"complete_state", observation.complete},
                   {"unobserved", observation.unobserved},
                   {"subsystems", names}});
        }
        Json hashes = Json::object();
        Json unavailable = Json::object();
        for (const auto& subsystem : observation.subsystems) {
            if (subsystem.digest) {
                hashes[subsystem.name] = hex(*subsystem.digest);
            } else {
                unavailable[subsystem.name] = subsystem.unavailable;
            }
        }
        write({{"type", "tick"},
               {"tick", tick},
               {"input_digest", digest(*bytes)},
               {"subsystems", hashes},
               {"unavailable", unavailable}});
    }
    write({{"type", "complete"}, {"ticks", options.ticks}, {"last_tick", options.ticks}});
    report.close();
    require(!report.fail(), "Could not finish report");
    if (network) {
        network->finish();
    }
    if (!options.timings.empty()) {
        const auto summary = [](std::span<const f64> samples) -> Json {
            const auto value = test::summarizeNativeTimings(samples);
            if (!value) {
                require(samples.empty(), "Invalid measured timing");
                return nullptr;
            }
            return {{"samples", value->samples}, {"mean", value->mean},
                    {"p50", value->p50}, {"p95", value->p95}, {"p99", value->p99},
                    {"maximum", value->maximum}};
        };
        const Json timings{{"schema", 1}, {"units", "milliseconds"},
                           {"simulation", "PlayScene::update"}, {"tick_rate", 60},
                           {"stage", options.stage}, {"players", options.players},
                           {"workload", options.workload},
                           {"input_delivery", network ? "iroh-lockstep" : "local"},
                           {"assets_digest", assetsDigest}, {"data_digest", dataDigest},
                           {"config_digest", configDigest},
                           {"ticks", options.ticks}, {"warmup_ticks", options.timingWarmup},
                           {"render_every", options.renderEvery}, {"headless", true},
                           {"ready_ticks", readyTicks}, {"moved_ticks", movedTicks},
                           {"most_enemies", mostEnemies}, {"most_projectiles", mostProjectiles},
                           {"update", summary(updateTimes)},
                           {"diagnostic_observation", summary(observationTimes)},
                           {"headless_render", summary(renderTimes)},
                           {"save", nullptr}, {"restore", nullptr}, {"replay", nullptr},
                           {"complete_rewind", false}};
        std::ofstream timingReport(options.timings, std::ios::binary);
        require(timingReport.is_open(), "Could not open timing report");
        timingReport << timings.dump(2) << '\n';
        timingReport.close();
        require(!timingReport.fail(), "Could not finish timing report");
    }
    scene.close();
    std::cout << "Native simulation report: " << options.report << '\n';
    std::cout << "Native workload: ready_ticks=" << readyTicks << " moved_ticks=" << movedTicks
              << " most_enemies=" << mostEnemies << " most_player_projectiles=" << mostProjectiles
              << '\n';
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::string numericError;
        if (!NativeOnlineIdentity::prepareProcessNumeric(&numericError)) {
            throw std::runtime_error(numericError);
        }
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "nativeprobe --report path.jsonl [--stage G1|tower|...] [--ticks 600] "
                         "[--players 1..4] [--render-every 0..60] [--workload idle|combat] "
                         "[--assets path] [--data path] [--perturb-tick tick] "
                         "[--timings timings.json --timing-warmup ticks] "
                         "[--network-role host|guest --network-peer 1..4 --ticket path "
                         "--input-delay 0..6]\n"
                         "No desktop control, audio device, or persisted saves. "
                         "Partial state observation; not a full determinism certificate.\n";
            return 0;
        }
        run(parse({argv, static_cast<usize>(argc)}));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native simulation probe failed: " << error.what() << '\n';
        return 1;
    }
}
