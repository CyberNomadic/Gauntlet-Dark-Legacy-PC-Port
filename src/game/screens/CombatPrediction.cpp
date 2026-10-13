#include "game/screens/CombatPrediction.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "engine/core/Log.h"

#include "game/config/GameConfig.h"
#include "game/players/NameCheats.h"
#include "game/players/Progression.h"
#include "game/screens/CombatResources.h"
#include "game/screens/OpponentResources.h"
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
    LevelOpponents opponents;
    PlayerHealth health;
    LevelWatch levels;
    std::vector<PlayerRuntime> party;
    CombatResources resources;
    std::vector<std::unique_ptr<PlayerRuntimeCheckpoint>> participants;
    std::unique_ptr<CombatRollbackCheckpoint> combat;
    std::unique_ptr<OrdinaryCombatCheckpoint> ordinary;
    std::unique_ptr<OrdinaryCombatTick> runner;
    std::optional<OrdinaryCombatCheckpoint::State> authority;
    MatchContext context;
    u64 baseline = 0;
    u64 tick = 0;
    bool active = false;

    ~Data() {
        runner.reset();
        ordinary.reset();
        combat.reset();
        participants.clear();
        attacks.clear();
        arsenal.clear();
        fixtures.clear();
        effects.clear();
        resources.clear();
        opponents.close();
    }

    bool load(RenderDevice& device, const std::filesystem::path& root, const LevelRef& level,
              const MatchContext& match, const PartyBootstrap::Party& profiles, f32 gain) {
        if (!world.load(device, root, level) || !classes.load(root / "pdata") ||
            !weapons.load(root / "WEAPONS") || !common.load(root / "STATIC")) {
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
        opponents.open({device, world, weapons, effects, audio, root, gain, true}, party);
        if (!StageResources::preload(world, opponents.enemies(), opponents.critters(),
                                     opponents.bosses())) {
            return false;
        }
        arsenal.bind({device, classes, weapons, world.collision(), effects, audio, nullptr,
                      world.wallHitSound(), world.isTower(),
                      world.level() != nullptr && world.level()->bossType >= 0, &world.powerups()},
                     lenders);
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
        if (!resources.bindStage(device, world, weapons, classes, party, arsenal, opponents,
                                 effects)) {
            return false;
        }
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
        return true;
    }

    void resetReplay() {
        if (!active) {
            return;
        }
        active = false;
        if (authority) {
            // The admitted runner cannot replace resources or external owners.
            // Failure to undo here is an ownership bug, never partial success.
            if (!resources.matches(party) || !ordinary->restore(*authority)) {
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
    return true;
}

void CombatPrediction::clear() {
    m_data.reset();
}
bool CombatPrediction::loaded() const {
    return m_data != nullptr;
}

bool CombatPrediction::begin(u64 epoch, u64 tick,
                             const OrdinaryCombatCheckpoint::State& authority) {
    if (!m_data || !m_data->resources.matches(m_data->party)) {
        return false;
    }
    auto& data = *m_data;
    data.resetReplay();
    if (epoch != data.context.epoch || (data.authority && tick < data.baseline) ||
        !authority.valid() || authority.combat.players.size() != data.party.size()) {
        return false;
    }
    // Copy first: allocation failure cannot leave the owner at a new boundary
    // without its associated undo image.
    auto next = authority;
    if (!data.ordinary->restore(next)) {
        return false;
    }
    data.authority = std::move(next);
    data.baseline = tick;
    data.tick = tick;
    data.active = true;
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
        auto result = data.runner->step(data.party, inputs, frame);
        if (result.status != OrdinaryCombatTick::Status::Advanced) {
            data.resetReplay();
            return result;
        }
        data.tick = tick;
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
    return m_data && m_data->resources.matches(m_data->party) ? m_data->ordinary->checkpoint()
                                                              : std::nullopt;
}

} // namespace gdl::game
