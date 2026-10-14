#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/StageStatePacket.h"
#include "game/screens/CombatCapture.h"
#include "game/screens/CombatReplayWorker.h"
#include "game/screens/StageParticlePresentation.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Worker = CombatReplayWorker;
using namespace std::chrono_literals;

MatchContext context() {
    MatchContext result;
    result.epoch = 7;
    result.scene = 1;
    result.owners = {1, 1, 1, 1};
    result.grants = {1, 2, 3, 4};
    return result;
}

// Scheduler-only envelopes are deliberately not native seeds. The executor
// below tests transport-independent coalescing; the native fixture later proves
// the actual validation/replay/publication path with loaded game resources.
std::unique_ptr<CombatSnapshot> envelope(const MatchContext& match, u64 tick) {
    auto result = std::make_unique<CombatSnapshot>();
    result->motion.epoch = match.epoch;
    result->motion.tick = tick;
    result->motion.cameraContinuity = 1;
    for (usize seat = 0; seat < match.owners.size(); ++seat) {
        if (match.owners[seat] == 0) {
            continue;
        }
        result->motion.players[seat] = SeatMotion{match.grants[seat], 1, {}, 0};
        result->players[seat].emplace();
    }
    result->opponents.emplace();
    result->opponentFrame.emplace();
    result->combatFrame.emplace();
    result->combatState.emplace().stage = std::vector<u8>{0};
    return result;
}

InputCommand command(const MatchContext& match, u64 tick, u8 seat = 0) {
    InputCommand result;
    result.epoch = match.epoch;
    result.tick = tick;
    result.seat = seat;
    result.grant = match.grants[seat];
    return result;
}

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Worker::Request> requests;
    std::thread::id producer;
    usize permitted = 0;
    bool released = false;
    bool throws = false;
    usize finishes = 0;

    Worker::Executor executor() {
        return {
            [this](const Worker::Request& request) {
                std::unique_lock lock{mutex};
                producer = std::this_thread::get_id();
                requests.push_back(request);
                const auto number = requests.size();
                changed.notify_all();
                changed.wait(lock, [&] { return released || permitted >= number; });
                if (throws) {
                    throw std::runtime_error{"bounded executor failure"};
                }
                Worker::Output output;
                auto counters = std::make_shared<OnlineCombatReplay::Diagnostics>();
                counters->domains[static_cast<usize>(OnlineCombatReplay::Domain::Stage)].updates =
                    number;
                output.diagnostics = std::move(counters);
                return output;
            },
            [this] {
                std::lock_guard lock{mutex};
                ++finishes;
                changed.notify_all();
            }};
    }
    bool wait(usize count) {
        std::unique_lock lock{mutex};
        return changed.wait_for(lock, 5s, [&] { return requests.size() >= count; });
    }
    Worker::Request request(usize index) {
        std::lock_guard lock{mutex};
        return requests.at(index);
    }
    void permit(usize count) {
        std::lock_guard lock{mutex};
        permitted = count;
        changed.notify_all();
    }
    void release() {
        std::lock_guard lock{mutex};
        released = true;
        changed.notify_all();
    }
};
// Catch assertion unwinding must release a blocked producer before ~Worker joins.
struct ReleaseGate {
    Gate& gate;
    ~ReleaseGate() { gate.release(); }
};

std::shared_ptr<const Worker::Completion> completion(Worker& worker) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = worker.take()) {
            return result;
        }
        if (!worker.queueState().running) {
            return worker.take();
        }
        std::this_thread::yield();
    }
    return nullptr;
}

TEST_CASE("combat worker coalesces authority without losing sparse four-seat input edges",
          "[rollback][combat-replay-worker]") {
    const auto match = context();
    auto authority = envelope(match, 100);
    const auto lifetime = std::make_shared<const u8>(0);
    Gate gate;
    Worker worker;
    ReleaseGate release{gate};
    REQUIRE(worker.start(match, 1, lifetime, gate.executor()));
    std::vector<InputCommand> first;
    for (u8 seat = 0; seat < 4; ++seat) {
        auto input = command(match, 105, seat);
        input.pressedButtons = static_cast<u32>(CommandPress::Attack);
        first.push_back(input);
    }
    REQUIRE(worker.submit(authority.get(), first) == Worker::Submission::Queued);
    REQUIRE(gate.wait(1));
    CHECK(gate.producer != std::this_thread::get_id());
    const auto original = gate.request(0);
    CHECK(original.authority.get() != authority.get());
    authority->motion.tick = 101;
    REQUIRE(worker.submit(authority.get(), {}) == Worker::Submission::Queued);
    std::vector<InputCommand> held;
    for (u8 seat = 0; seat < 4; ++seat) {
        auto input = command(match, 110, seat);
        input.heldButtons = static_cast<u32>(CommandHeld::ThrowPotion);
        held.push_back(input);
    }
    REQUIRE(worker.submit(nullptr, held) == Worker::Submission::Queued);
    authority->motion.tick = 102;
    std::vector<InputCommand> released;
    for (u8 seat = 0; seat < 4; ++seat) {
        released.push_back(command(match, 112, seat));
    }
    REQUIRE(worker.submit(authority.get(), released) == Worker::Submission::Queued);
    REQUIRE(worker.submit(authority.get(), released) == Worker::Submission::Unchanged);
    auto state = worker.queueState();
    CHECK(state.inFlight);
    CHECK(state.pending);
    CHECK(state.retainedCommands == 12);
    CHECK(state.coalesced == 2);
    CHECK(original.authority->motion.tick == 100);
    CHECK(lifetime.use_count() == 1);
    gate.permit(1);
    REQUIRE(gate.wait(2));
    const auto superseded = worker.take();
    REQUIRE(superseded);
    CHECK(superseded->status == Worker::Status::Superseded);
    CHECK_FALSE(superseded->publication);
    CHECK(superseded->authorityTick == 100);
    const auto latest = gate.request(1);
    REQUIRE(latest.authority->motion.tick == 102);
    REQUIRE(latest.commands.size() == 12);
    for (usize index = 0; index < latest.commands.size(); ++index) {
        const auto& input = latest.commands[index];
        CHECK(input.seat == index % 4);
        CHECK(input.epoch == match.epoch);
        CHECK(input.grant == match.grants[input.seat]);
        constexpr std::array<u64, 3> ticks{105, 110, 112};
        CHECK(input.tick == ticks[index / 4]);
        CHECK(input.pressed(CommandPress::Attack) == (index < 4));
        CHECK(input.held(CommandHeld::ThrowPotion) == (index >= 4 && index < 8));
    }
    gate.permit(2);
    const auto final = completion(worker);
    REQUIRE(final);
    CHECK(final->status == Worker::Status::Unsupported);
    CHECK(final->authorityTick == 102);
    CHECK(final->inputThroughTick == 112);
    REQUIRE(final->diagnostics);
    CHECK(final->diagnostics->domains[2].updates == 2);
    // Consumed is not acknowledged. All inputs remain until real authority.
    CHECK(worker.queueState().retainedCommands == 12);
    worker.stop();
    CHECK(gate.finishes == 1);
    CHECK(final->diagnostics->domains[2].updates == 2);
}

TEST_CASE("combat worker journal is bounded and rejected submissions are atomic",
          "[rollback][combat-replay-worker]") {
    const auto match = context();
    auto authority = envelope(match, 100);
    const auto lifetime = std::make_shared<const u8>(0);
    Gate gate;
    Worker worker;
    ReleaseGate release{gate};
    REQUIRE(worker.start(match, 1, lifetime, gate.executor()));
    REQUIRE(worker.submit(authority.get(), {}) == Worker::Submission::Queued);
    REQUIRE(gate.wait(1));
    std::vector<InputCommand> commands;
    for (u64 tick = 101; tick <= 220; ++tick) {
        for (u8 seat = 0; seat < 4; ++seat) {
            commands.push_back(command(match, tick, seat));
        }
    }
    REQUIRE(commands.size() == Worker::kMaxCommands);
    REQUIRE(worker.submit(nullptr, commands) == Worker::Submission::Queued);
    const auto before = worker.queueState();
    auto extra = command(match, 221);
    CHECK(worker.submit(nullptr, std::span{&extra, 1}) == Worker::Submission::Capacity);
    extra = commands.front();
    extra.pressedButtons = static_cast<u32>(CommandPress::UsePotion);
    CHECK(worker.submit(nullptr, std::span{&extra, 1}) == Worker::Submission::Conflict);
    extra = commands.front();
    ++extra.epoch;
    CHECK(worker.submit(nullptr, std::span{&extra, 1}) == Worker::Submission::Input);
    extra = commands.front();
    ++extra.grant;
    CHECK(worker.submit(nullptr, std::span{&extra, 1}) == Worker::Submission::Input);
    authority->motion.tick = 99;
    CHECK(worker.submit(authority.get(), {}) == Worker::Submission::Authority);
    authority->motion.tick = 160;
    authority->combatState->stage.reset();
    CHECK(worker.submit(authority.get(), {}) == Worker::Submission::Authority);
    authority->combatState->stage = std::vector<u8>{0};
    CHECK(worker.queueState().submitted == before.submitted);
    CHECK(worker.queueState().retainedCommands == Worker::kMaxCommands);
    REQUIRE(worker.submit(authority.get(), {}) == Worker::Submission::Queued);
    CHECK(worker.queueState().retainedCommands == 60 * 4);
    gate.permit(1);
    REQUIRE(gate.wait(2));
    const auto second = gate.request(1);
    REQUIRE(second.commands.size() == 60 * 4);
    CHECK(second.authority->motion.tick == 160);
    CHECK(second.commands.front().tick == 161);
    CHECK(second.commands.back().tick == 220);
}

TEST_CASE("combat worker stop joins an in-flight transaction and discards pending work",
          "[rollback][combat-replay-worker]") {
    const auto match = context();
    auto authority = envelope(match, 100);
    const auto lifetime = std::make_shared<const u8>(0);
    Gate gate;
    Worker worker;
    ReleaseGate release{gate};
    REQUIRE(worker.start(match, 1, lifetime, gate.executor()));
    REQUIRE(worker.submit(authority.get(), {}) == Worker::Submission::Queued);
    REQUIRE(gate.wait(1));
    authority->motion.tick = 101;
    REQUIRE(worker.submit(authority.get(), {}) == Worker::Submission::Queued);
    std::promise<void> entered;
    auto stopping = std::async(std::launch::async, [&] {
        entered.set_value();
        worker.stop();
    });
    entered.get_future().wait();
    // The producer is still blocked in its first run, so pending can clear only
    // when stop has acquired the scheduler mutex. No native/resource teardown
    // can occur until the gate is released below.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (worker.queueState().pending && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    CHECK_FALSE(worker.queueState().pending);
    // The pass cannot finish until released, regardless of scheduling speed.
    CHECK(stopping.wait_for(0ms) == std::future_status::timeout);
    gate.release();
    REQUIRE(stopping.wait_for(5s) == std::future_status::ready);
    stopping.get();
    CHECK(gate.requests.size() == 1);
    CHECK(gate.finishes == 1);
    CHECK_FALSE(worker.queueState().running);
    CHECK_FALSE(worker.take());
    CHECK(worker.submit(authority.get(), {}) == Worker::Submission::Stopped);
}

TEST_CASE("combat worker exceptions and expired resource fences cannot publish success",
          "[rollback][combat-replay-worker]") {
    const auto match = context();
    auto authority = envelope(match, 100);
    auto lifetime = std::make_shared<const u8>(0);
    Gate gate;
    Worker worker;
    ReleaseGate release{gate};
    SECTION("executor exception") {
        gate.throws = true;
    }
    SECTION("resource invalidation") {}
    REQUIRE(worker.start(match, 1, lifetime, gate.executor()));
    REQUIRE(worker.submit(authority.get(), {}) == Worker::Submission::Queued);
    REQUIRE(gate.wait(1));
    if (!gate.throws) {
        lifetime.reset(); // marker only; no real native owner is destroyed here
    }
    gate.release();
    const auto result = completion(worker);
    REQUIRE(result);
    CHECK(result->status == (gate.throws ? Worker::Status::Exception : Worker::Status::Resources));
    CHECK_FALSE(result->publication);
    worker.stop();
    CHECK(gate.finishes == 1);
    CHECK_FALSE(worker.queueState().running);
}

TEST_CASE("combat worker restart isolates epoch grants and foreign local input",
          "[rollback][combat-replay-worker]") {
    auto match = context();
    match.owners = {1, 2, 0, 0};
    match.grants = {1, 2, 0, 0};
    auto authority = envelope(match, 100);
    const auto lifetime = std::make_shared<const u8>(0);
    Gate gate;
    Worker worker;
    ReleaseGate release{gate};
    REQUIRE(worker.start(match, 2, lifetime, gate.executor()));
    auto input = command(match, 101, 0);
    CHECK(worker.submit(authority.get(), std::span{&input, 1}) == Worker::Submission::Input);
    CHECK(worker.queueState().submitted == 0);
    input = command(match, 101, 1);
    REQUIRE(worker.submit(authority.get(), std::span{&input, 1}) == Worker::Submission::Queued);
    REQUIRE(gate.wait(1));
    gate.release();
    REQUIRE(completion(worker));
    worker.stop();
    CHECK(gate.finishes == 1);
    ++match.epoch;
    ++match.grants[1];
    REQUIRE(worker.start(match, 2, lifetime, gate.executor()));
    CHECK(worker.submit(authority.get(), {}) == Worker::Submission::Authority);
    authority = envelope(match, 200);
    CHECK(worker.submit(authority.get(), std::span{&input, 1}) == Worker::Submission::Input);
    input = command(match, 201, 1);
    REQUIRE(worker.submit(authority.get(), std::span{&input, 1}) == Worker::Submission::Queued);
    const auto result = completion(worker);
    REQUIRE(result);
    CHECK(result->context == match);
    CHECK(result->request == 1);
    CHECK(result->authorityTick == 200);
    CHECK(result->inputThroughTick == 201);
    CHECK(worker.queueState().retainedCommands == 1);
    worker.stop();
    CHECK(gate.finishes == 2);
}

struct NativeFixture {
    test::FakeRenderDevice device;
    MatchContext match;
    PartyBootstrap::Party profiles;
    std::unique_ptr<CombatPrediction> prediction = std::make_unique<CombatPrediction>();
    std::unique_ptr<CombatSnapshot> baseline = std::make_unique<CombatSnapshot>();

    NativeFixture() {
        const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
        const auto directory = test::sampleLevel("worker-native-stage");
        writeTextFile(directory / "world.json", R"({"objects":[
          {"name":"FLOOR","position":[0,0,0],"flags":4}]})");
        writeTextFile(directory / "collision.json", R"({"objects":[
          {"object":0,"normals":[0,1,0,0,1,0],"vertices":[
          -200,0,-200,200,0,-200,200,0,200,-200,0,-200,200,0,200,-200,0,200]}]})");
        match.epoch = 3;
        match.scene = 1;
        match.owners = {1, 2, 0, 0};
        match.grants = {2, 5, 0, 0};
        for (usize seat = 0; seat < 2; ++seat) {
            CharacterSave save;
            save.name = "TEST";
            save.color = static_cast<s32>(seat);
            save.autoAttack = false;
            profiles[seat] = CharacterProfile::capture(save);
            REQUIRE(profiles[seat]);
        }
        REQUIRE(prediction->open(
            device, root,
            {.name = "TEST", .directory = directory.string(), .items = "ITEMS/levelG"}, match,
            profiles, 1));
        auto seed = std::make_unique<OrdinaryCombatCheckpoint::State>();
        {
            auto captured = prediction->checkpoint(true);
            REQUIRE(captured);
            *seed = std::move(*captured);
        }
        seed->combat.players[0].actor.position = {-12, 0, 0};
        seed->combat.players[1].actor.position = {12, 0, 0};
        REQUIRE(prediction->begin(match.epoch, 100, *seed));
        OrdinaryCombatTick::Frame frame;
        frame.camera.position = {0, 35, -50};
        frame.camera.pitch = .6f;
        frame.projection.horizontalFov = 1.6f;
        frame.projection.aspect = 2;
        MotionSnapshot motion;
        motion.epoch = match.epoch;
        motion.tick = 100;
        motion.cameraContinuity = 1;
        motion.camera = frame.camera;
        motion.horizontalFov = frame.projection.horizontalFov;
        motion.aspect = frame.projection.aspect;
        for (const auto& player : prediction->players()) {
            const auto seat = static_cast<usize>(player.actor.player());
            motion.players[seat] =
                SeatMotion{match.grants[seat], 1, player.actor.position(), player.actor.yaw()};
        }
        {
            auto captured =
                CombatCapture::capture(motion, prediction->players(), *prediction->enemies(),
                                       nullptr, prediction->world());
            REQUIRE(captured);
            *baseline = std::move(*captured);
        }
        baseline->geometry = prediction->world()->scene().geometry();
        baseline->opponents = seed->opponents;
        auto& opponents = baseline->opponentFrame.emplace();
        opponents.initialPlayers = 2;
        for (const auto& view : LevelOpponents::enemyViews(prediction->players())) {
            opponents.players.push_back({static_cast<u8>(view.player), view});
        }
        baseline->combatState = CombatStatePacket::State{seed->combat, seed->adapter};
        const auto catalog = StageStatePacket::Catalog::build(*seed->stage);
        REQUIRE(catalog);
        baseline->combatState->stage = StageStatePacket::encode(*seed->stage, *catalog);
        REQUIRE(baseline->combatState->stage);
        baseline->combatFrame = frame;
        REQUIRE(baseline->valid());
    }
};

std::vector<u8> sample(const StageReplayPresentation::Publication& publication,
                       const CombatSnapshot& baseline,
                       const std::shared_ptr<const StageReplayPresentation::Publication>& owner) {
    StageReplayPresentation presenter;
    const StageReplayPresentation::Time start{1s};
    REQUIRE(presenter.adopt(owner, publication.authority(), start));
    auto shown = std::make_unique<CombatSnapshot>(baseline);
    EnemyPresentation presentation;
    presentation.epoch = shown->motion.epoch;
    presentation.tick = shown->motion.tick;
    presentation.shownGeometry = shown->geometry;
    REQUIRE(presenter.present(*shown, presentation, publication.authority(), start + 20ms));
    REQUIRE(presentation.particles);
    const auto encoded = CombatPacket::encode(*shown);
    REQUIRE(encoded);
    return *encoded;
}

TEST_CASE("native Stage worker matches synchronous replay without renderer or asset access",
          "[rollback][combat-replay-worker][assets]") {
    auto fixture = std::make_unique<NativeFixture>();
    auto input = command(fixture->match, 102, 1);
    input.direction = {1, 0};
    input.magnitude = .5f;
    OnlineCombatReplay synchronous;
    REQUIRE(synchronous.begin(fixture->match, 2, *fixture->prediction));
    REQUIRE(synchronous.update(fixture->baseline.get(), std::span{&input, 1}));
    const auto expectedPublication = synchronous.publication();
    REQUIRE(expectedPublication);
    const auto expected = sample(*expectedPublication, *fixture->baseline, expectedPublication);
    synchronous.clear();
    const auto uploads = fixture->device.texturesCreated;
    const auto updates = fixture->device.textureUpdates;
    const auto draws = fixture->device.draws.size();
    const auto lifetime = fixture->prediction->resourceLifetime();
    Worker worker;
    REQUIRE(worker.start(fixture->match, 2, *fixture->prediction));
    REQUIRE(worker.submit(fixture->baseline.get(), std::span{&input, 1}) ==
            Worker::Submission::Queued);
    const auto first = completion(worker);
    REQUIRE(first);
    REQUIRE(first->status == Worker::Status::Published);
    REQUIRE(first->publication);
    CHECK((sample(*first->publication, *fixture->baseline, first->publication) == expected));
    // Only detached values are touched while the producer owns prediction.
    const auto retained = first->publication;
    input.tick = 103;
    REQUIRE(worker.submit(nullptr, std::span{&input, 1}) == Worker::Submission::Queued);
    const auto second = completion(worker);
    REQUIRE(second);
    REQUIRE(second->status == Worker::Status::Published);
    CHECK(second->publication->tick() == 103);
    CHECK((sample(*retained, *fixture->baseline, retained) == expected));
    REQUIRE(second->diagnostics);
    CHECK(second->diagnostics->domains[2].appendPasses == 1);
    input.tick = 104;
    input.pressedButtons = static_cast<u32>(CommandPress::SelectorUp);
    REQUIRE(worker.submit(nullptr, std::span{&input, 1}) == Worker::Submission::Queued);
    const auto refused = completion(worker);
    REQUIRE(refused);
    CHECK(refused->status == Worker::Status::Unsupported);
    CHECK_FALSE(refused->publication);
    REQUIRE(refused->diagnostics);
    CHECK(refused->diagnostics->domains[2].refusals > second->diagnostics->domains[2].refusals);
    worker.stop();
    // Joining returns native access to main and rewinds the complete pass.
    CHECK_FALSE(fixture->prediction->active());
    CHECK(fixture->device.texturesCreated == uploads);
    CHECK(fixture->device.textureUpdates == updates);
    CHECK(fixture->device.draws.size() == draws);
    CHECK((sample(*retained, *fixture->baseline, retained) == expected));
    StageReplayPresentation presenter;
    REQUIRE(presenter.adopt(retained, retained->authority()));
    presenter.clear(); // publications below are fences, not strong artwork leases
    fixture->prediction->clear();
    CHECK(lifetime.expired());
    CHECK_FALSE(presenter.adopt(retained, retained->authority()));
}
} // namespace
