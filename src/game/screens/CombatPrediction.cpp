#include "game/screens/CombatPrediction.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "engine/core/Log.h"

#include "game/config/GameConfig.h"
#include "game/netplay/StageStatePacket.h"
#include "game/players/NameCheats.h"
#include "game/players/Progression.h"
#include "game/screens/CombatResources.h"
#include "game/screens/OpponentResources.h"
#include "game/screens/StageCombatPresentation.h"
#include "game/screens/StageParticlePresentation.h"
#include "game/screens/StageResources.h"

namespace gdl::game {

struct CombatPrediction::Data {
    LevelWorld world;
    ClassDataSet classes;
    ItemArchive weapons;
    TextureSet common;
    LevelSoundscape audio;
    EffectTrees effects;
    AmbientDimmer dimmer;
    PlayerArsenal arsenal;
    PlayerAttacks attacks;
    LevelFixtures fixtures;
    ExitPortals portals;
    LevelCatalog levelsCatalog;
    LevelOpponents opponents;
    PlayerHealth health;
    LevelWatch levels;
    std::vector<PlayerRuntime> party;
    CombatResources resources;
    ProjectileResources projectilePictures;
    PickupResources pickupPictures;
    FixtureResources fixturePictures;
    FighterResources fighterPictures;
    bool picturesReady = false;
    std::shared_ptr<const u8> lifetime = std::make_shared<const u8>(0);
    std::vector<std::unique_ptr<PlayerRuntimeCheckpoint>> participants;
    std::unique_ptr<CombatRollbackCheckpoint> combat;
    std::unique_ptr<OrdinaryCombatCheckpoint> ordinary;
    std::unique_ptr<OrdinaryCombatTick> runner;
    std::unique_ptr<LevelWorldCheckpoint> stageWorld;
    std::unique_ptr<LevelFixturesCheckpoint> stageFixtures;
    TowerCamera camera;
    std::unique_ptr<TowerCameraCheckpoint> stageCamera;
    std::unique_ptr<ExitPortalsCheckpoint> stagePortals;
    std::unique_ptr<OrdinaryCombatCheckpoint> staged;
    std::unique_ptr<OrdinaryCombatTick> stagedRunner;
    StageStatePacket::Catalog stageCatalog;
    std::unique_ptr<OrdinaryCombatCheckpoint::State> authority;
    MatchContext context;
    u64 baseline = 0;
    u64 tick = 0;
    u64 replayRevision = 1;
    bool active = false;
    bool stageSelected = false;

    OrdinaryCombatCheckpoint* checkpointOwner() const {
        return stageSelected ? staged.get() : ordinary.get();
    }

    ~Data() {
        lifetime.reset(); // Invalidate borrowed particle artwork before any owner releases it.
        stagedRunner.reset();
        staged.reset();
        stagePortals.reset();
        stageCamera.reset();
        stageFixtures.reset();
        stageWorld.reset();
        runner.reset();
        ordinary.reset();
        combat.reset();
        participants.clear();
        attacks.clear();
        arsenal.clear();
        fixtures.clear();
        portals.clear();
        effects.clear();
        resources.clear();
        opponents.close();
    }

    bool load(RenderDevice& device, const std::filesystem::path& root, const LevelRef& level,
              const MatchContext& match, const PartyBootstrap::Party& profiles, f32 gain) {
        if (!world.load(device, root, level) || !classes.load(root / "pdata") ||
            !weapons.load(root / "WEAPONS") || !common.load(root / "STATIC") ||
            !levelsCatalog.load(root)) {
            return false;
        }
        context = match;
        party.reserve(profiles.size());
        for (usize seat = 0; seat < profiles.size(); ++seat) {
            if (!profiles[seat]) {
                continue;
            }
            auto save = profiles[seat]->gameplayCopy();
            applyNameCheats(save);
            const auto* stats = classes.stats(save.character);
            if (stats == nullptr) {
                return false;
            }
            PlayerRuntime player;
            // No gameplay runs until authority supplies the complete placement,
            // inventory, entry record, figure pose and reaction state.
            player.actor.spawn(static_cast<s32>(seat), save, stats, Vec3{0}, 0);
            player.entrySave = save;
            player.figure = PlayerFigure::load(device, root, save, false);
            if (!player.figure) {
                return false;
            }
            levels.observe(static_cast<s32>(seat), experienceLevel(save.experience()));
            party.push_back(std::move(player));
        }
        const auto count = static_cast<s32>(party.size());
        world.setPlayerCount(count);
        const std::array<TextureSet*, 5> lenders{&weapons.textures, &world.items().textures,
                                                 &world.realmItems().textures,
                                                 &world.powerups().textures, &common};
        effects.setTextureLenders(lenders);
        fixtures.bind({device, world, weapons, effects, audio, gain});
        fixtures.setPlayerCount(count);
        portals.bind(device, world.layout(), world.itemScope(), levelsCatalog, &world.collision());
        opponents.open({device, world, weapons, effects, audio, root, gain, true}, party);
        // Match the native scene's borrowed animation resources, including levels
        // with no safe rocks. Checkpoints retain this binding independently of
        // the current rock roster.
        ItemArchive* bossArchive = opponents.bosses().archive();
        const std::array<TextureSet*, 3> arenaTextures{
            &world.textures(), &world.realmItems().textures,
            bossArchive != nullptr ? &bossArchive->textures : nullptr};
        fixtures.safeRocks().bindAnimations(device, world.items(), arenaTextures);
        if (opponents.bosses().raisesArenaRocks()) {
            fixtures.safeRocks().hideForEruptions();
        }
        if (!StageResources::preload(world, opponents.enemies(), opponents.critters(),
                                     opponents.bosses())) {
            return false;
        }
        arsenal.bind({device, classes, weapons, world.collision(), effects, audio, nullptr,
                      world.wallHitSound(), world.isTower(),
                      world.level() != nullptr && world.level()->bossType >= 0, &world.powerups()},
                     lenders);
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer,
                      nullptr, nullptr,
                      gain == DifficultyConfig::kGains.front() ? TargetAssist::kEasyFacingDot
                                                               : TargetAssist::kFacingDot});
        if (!resources.bindStage(device, world, weapons, classes, party, arsenal, opponents,
                                 effects)) {
            return false;
        }
        const auto fixtureArchives = StageResources::fixtureArchives(
            world, opponents.enemies(), opponents.critters(), opponents.bosses());
        std::array<PlayerFigure*, InputCommand::kSeats> figures{};
        for (auto& player : party) {
            figures[static_cast<usize>(player.actor.player())] = player.figure.get();
        }
        auto stocks = opponents.critters().resources();
        for (auto* stock : opponents.bosses().resources()) {
            stocks.push_back(stock);
        }
        const std::array archives{&world.items(), &world.realmItems(), &world.powerups()};
        picturesReady = projectilePictures.addPlayers(device, weapons, figures, lenders) &&
                        projectilePictures.addEnemies(device, opponents.enemies(), lenders) &&
                        projectilePictures.addStage(device, archives, stocks, lenders) &&
                        pickupPictures.bind(device, world.placedItems().archives()) &&
                        fixturePictures.bind(device, fixtureArchives, opponents.generators(),
                                             fixtures.safeRocks(), &opponents.statues()) &&
                        fighterPictures.bind(opponents.critters(), opponents.bosses());
        std::vector<PlayerRuntimeCheckpoint*> views;
        for (auto& player : party) {
            participants.push_back(std::make_unique<PlayerRuntimeCheckpoint>(
                player, classes.stats(player.actor.save().character)));
            views.push_back(participants.back().get());
        }
        combat = std::make_unique<CombatRollbackCheckpoint>(views, health, levels, attacks, arsenal,
                                                            effects, resources.resources());
        ordinary = std::make_unique<OrdinaryCombatCheckpoint>(
            *combat, opponents, OpponentResources::missiles(opponents.enemies()));
        runner = std::make_unique<OrdinaryCombatTick>(*ordinary, classes, fixtures);
        // Catalog creation belongs to stage loading, never a received seed or a
        // replay tick. A stage unsupported by these optional owners retains the
        // existing combat-only domain; a supplied Stage is never downgraded.
        auto nextWorld = std::make_unique<LevelWorldCheckpoint>(world);
        auto nextFixtures = std::make_unique<LevelFixturesCheckpoint>(fixtures);
        std::vector<CameraSubject> subjects;
        for (const auto& player : party) {
            subjects.push_back(
                {player.actor.position(), player.actor.followPoint(), player.actor.height() * .5f});
        }
        camera.reset(subjects, world.cameraMarkers(), world.cameraRange(), {});
        auto nextCamera = std::make_unique<TowerCameraCheckpoint>(camera, world);
        auto nextPortals = std::make_unique<ExitPortalsCheckpoint>(
            portals, world.layout(), world.items(), &world.realmItems());
        if (nextWorld->bindResources(device) && nextFixtures->bindResources() &&
            nextPortals->bindResources()) {
            auto worldState = nextWorld->checkpoint();
            auto fixtureState = nextFixtures->checkpoint();
            auto cameraState = nextCamera->checkpoint();
            auto portalState = nextPortals->checkpoint();
            if (worldState && fixtureState && cameraState && portalState) {
                auto catalog = StageStatePacket::Catalog::build(
                    {std::move(*worldState), std::move(*fixtureState), std::move(*cameraState),
                     std::move(*portalState)});
                if (catalog) {
                    stageCatalog = std::move(*catalog);
                    stageWorld = std::move(nextWorld);
                    stageFixtures = std::move(nextFixtures);
                    stageCamera = std::move(nextCamera);
                    stagePortals = std::move(nextPortals);
                    staged = std::make_unique<OrdinaryCombatCheckpoint>(
                        *combat, opponents, OpponentResources::missiles(opponents.enemies()),
                        stageWorld.get(), stageFixtures.get(), stageCamera.get(),
                        stagePortals.get());
                    stagedRunner = std::make_unique<OrdinaryCombatTick>(*staged, classes, fixtures);
                }
            }
        }
        return true;
    }

    void resetReplay() {
        if (!active) {
            return;
        }
        active = false;
        ++replayRevision;
        if (authority) {
            // The admitted runner cannot replace resources or external owners.
            // Failure to undo here is an ownership bug, never partial success.
            if (!resources.matches(party) || !checkpointOwner()->restore(*authority)) {
                std::terminate();
            }
            tick = baseline;
        }
    }
};

CombatPrediction::CombatPrediction() = default;
CombatPrediction::~CombatPrediction() = default;

bool CombatPrediction::open(RenderDevice& device, const std::filesystem::path& root,
                            const LevelRef& level, const MatchContext& context,
                            const PartyBootstrap::Party& party, f32 difficultyGain) {
    if (!context.valid() || (m_data && context.epoch <= m_data->context.epoch) ||
        context.transition == MatchTransition::Resume ||
        std::ranges::find(DifficultyConfig::kGains, difficultyGain) ==
            DifficultyConfig::kGains.end()) {
        return false;
    }
    for (usize seat = 0; seat < party.size(); ++seat) {
        if (party[seat].has_value() != (context.owners[seat] != 0) ||
            (party[seat] && !party[seat]->valid())) {
            return false;
        }
    }
    try {
        auto next = std::make_unique<Data>();
        if (!next->load(device, root, level, context, party, difficultyGain)) {
            return false;
        }
        m_data = std::move(next);
        return true;
    } catch (const std::exception& error) {
        log::warn("Private combat resources: {}", error.what());
        return false;
    }
}

bool CombatPrediction::resume(const MatchContext& context) {
    if (!m_data || !m_data->resources.matches(m_data->party) || !context.valid() ||
        context.transition != MatchTransition::Resume || context.epoch <= m_data->context.epoch ||
        context.scene != m_data->context.scene || context.tickRate != m_data->context.tickRate ||
        context.owners != m_data->context.owners || context.grants != m_data->context.grants) {
        return false;
    }
    m_data->resetReplay();
    m_data->authority.reset();
    m_data->baseline = m_data->tick = 0;
    m_data->context = context;
    ++m_data->replayRevision;
    return true;
}

void CombatPrediction::clear() {
    m_data.reset();
}
bool CombatPrediction::loaded() const {
    return m_data != nullptr;
}
std::weak_ptr<const u8> CombatPrediction::resourceLifetime() const {
    return m_data ? m_data->lifetime : std::weak_ptr<const u8>{};
}

bool CombatPrediction::begin(u64 epoch, u64 tick, const OrdinaryCombatCheckpoint::State& authority,
                             OrdinaryCombatCheckpoint::RestoreIssue* issue) {
    using Issue = OrdinaryCombatCheckpoint::RestoreIssue;
    const auto reject = [&](Issue why) {
        if (issue) {
            *issue = why;
        }
        return false;
    };
    if (issue) {
        *issue = Issue::None;
    }
    if (!m_data || !m_data->resources.matches(m_data->party)) {
        return reject(Issue::Resources);
    }
    auto& data = *m_data;
    data.resetReplay();
    if (epoch != data.context.epoch || (data.authority && tick < data.baseline) ||
        authority.combat.players.size() != data.party.size()) {
        return reject(Issue::Context);
    }
    if (!authority.valid()) {
        return reject(Issue::State);
    }
    // Copy first: allocation failure cannot leave the owner at a new boundary
    // without its associated undo image.
    auto next = std::make_unique<OrdinaryCombatCheckpoint::State>(authority);
    auto* owner = next->stage ? data.staged.get() : data.ordinary.get();
    if (owner == nullptr) {
        return reject(Issue::Bindings);
    }
    if (!owner->restore(*next, issue)) {
        return false;
    }
    data.stageSelected = next->stage.has_value();
    data.authority = std::move(next);
    data.baseline = tick;
    data.tick = tick;
    data.active = true;
    ++data.replayRevision;
    return true;
}

OrdinaryCombatTick::Result CombatPrediction::advance(u64 tick, std::span<const PlayInput> inputs,
                                                     const OrdinaryCombatTick::Frame& frame) {
    if (!m_data || !m_data->resources.matches(m_data->party) || !m_data->active ||
        tick <= m_data->tick || tick - m_data->tick != 1 || tick - m_data->baseline > kMaxTicks ||
        frame.ticks != 60 / static_cast<s32>(m_data->context.tickRate)) {
        resetReplay();
        return {};
    }
    auto& data = *m_data;
    try {
        auto* runner = data.stageSelected ? data.stagedRunner.get() : data.runner.get();
        auto result = runner->step(data.party, inputs, frame);
        if (result.status != OrdinaryCombatTick::Status::Advanced) {
            data.resetReplay();
            return result;
        }
        data.tick = tick;
        ++data.replayRevision;
        return result;
    } catch (...) {
        data.resetReplay();
        throw;
    }
}

void CombatPrediction::resetReplay() {
    if (m_data) {
        m_data->resetReplay();
    }
}
bool CombatPrediction::active() const {
    return m_data && m_data->active;
}
u64 CombatPrediction::tick() const {
    return m_data ? m_data->tick : 0;
}
u64 CombatPrediction::replayRevision() const {
    return m_data ? m_data->replayRevision : 0;
}
std::span<const PlayerRuntime> CombatPrediction::players() const {
    return active() ? std::span<const PlayerRuntime>{m_data->party}
                    : std::span<const PlayerRuntime>{};
}
const LevelWorld* CombatPrediction::world() const {
    return m_data ? &m_data->world : nullptr;
}
const Enemies* CombatPrediction::enemies() const {
    return active() ? &m_data->opponents.enemies() : nullptr;
}
std::optional<OrdinaryCombatCheckpoint::State> CombatPrediction::checkpoint() const {
    return m_data && m_data->resources.matches(m_data->party)
               ? m_data->checkpointOwner()->checkpoint()
               : std::nullopt;
}
std::optional<OrdinaryCombatCheckpoint::State>
CombatPrediction::checkpoint(bool includeStage) const {
    if (!m_data || !m_data->resources.matches(m_data->party)) {
        return std::nullopt;
    }
    const auto* owner = includeStage ? m_data->staged.get() : m_data->ordinary.get();
    return owner != nullptr ? owner->checkpoint() : std::nullopt;
}
std::optional<OrdinaryCombatCheckpoint::Stage>
CombatPrediction::decodeStage(std::span<const u8> bytes) const {
    if (!m_data || !m_data->resources.matches(m_data->party) || !m_data->stageWorld ||
        !m_data->stageFixtures || !m_data->stageWorld->binds(m_data->world) ||
        !m_data->stageFixtures->binds(m_data->fixtures)) {
        return std::nullopt;
    }
    return StageStatePacket::decode(bytes, m_data->stageCatalog);
}

std::unique_ptr<CombatSnapshot>
CombatPrediction::stagePicture(const CombatSnapshot& authority, const MotionSnapshot& motion,
                               std::shared_ptr<const StageParticlePresentation>* particles) const {
    if (!active() || !m_data->stageSelected || !m_data->picturesReady || !m_data->stagePortals ||
        !m_data->resources.matches(m_data->party) || motion.epoch != m_data->context.epoch ||
        motion.tick != m_data->tick) {
        return nullptr;
    }
    auto& data = *m_data;
    auto figures =
        StageParticlePresentation::collectFigures(data.world, data.fixtures, data.opponents);
    if (!figures) {
        return nullptr;
    }
    auto nativeParticles = StageParticlePresentation::capture(
        motion.epoch, motion.tick, data.lifetime,
        {data.world, data.arsenal.missiles().visuals(), data.effects, &data.portals, *figures, true,
         &data.opponents.enemies(), data.party});
    if (!nativeParticles) {
        return nullptr;
    }
    auto picture = motion;
    picture.camera = data.camera.camera();
    auto captured = StageCombatPresentation::capture(
        authority, picture,
        {data.party,
         data.world,
         data.opponents.enemies(),
         data.arsenal.missiles(),
         data.opponents.missiles(),
         data.effects,
         {data.fixtures.chests(), data.fixtures.gates(), data.world.triggers(),
          data.opponents.generators(), data.fixtures.barrels(), data.fixtures.traps(),
          data.fixtures.safeRocks(), data.fixtures.rubble(), &data.opponents.statues(),
          &data.portals},
         data.opponents.critters(),
         data.opponents.bosses()},
        {data.projectilePictures, data.pickupPictures, data.fixturePictures, data.fighterPictures},
        nativeParticles.get());
    if (captured && particles != nullptr) {
        *particles = std::move(nativeParticles);
    }
    return captured;
}

} // namespace gdl::game
