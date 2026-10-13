#include "game/screens/ReplicaStage.h"

#include <algorithm>
#include <exception>

#include "engine/core/Log.h"

#include "game/screens/OpponentPrediction.h"
#include "game/screens/StageResources.h"

namespace gdl::game {
struct ReplicaStage::Assets {
    LevelWorld world;
    ItemArchive weapons;
    TextureSet common;
    Enemies enemies;
    Critters critters;
    Bosses bosses;
    Generators generators;
    SafeRocks rocks;
    ProjectileResources projectiles;
    PickupResources pickups;
    FixtureResources fixtures;
    ClassDataSet classes;
    std::array<std::optional<s32>, InputCommand::kSeats> characters;
    const Texture* flash = nullptr;
    OpponentPrediction prediction;
    // Renderers release all borrowed resources before their owners. The turbo
    // prototypes also borrow class archives held by the view's player figures.
    ReplicaView view;
    std::array<TurboEffectResources, InputCommand::kSeats> turboEffects;

    bool load(RenderDevice& device, const std::filesystem::path& root, const LevelRef& level,
              const MatchContext& context, const PartyBootstrap::Party& party,
              const StringTable* strings, f32 difficultyGain);
};
ReplicaStage::ReplicaStage() = default;
ReplicaStage::~ReplicaStage() = default;
bool ReplicaStage::Assets::load(RenderDevice& device, const std::filesystem::path& root,
                                const LevelRef& level, const MatchContext& context,
                                const PartyBootstrap::Party& party, const StringTable* strings,
                                f32 difficultyGain) {
    if (!world.load(device, root, level) || !weapons.load(root / "WEAPONS") ||
        !common.load(root / "STATIC")) {
        return false;
    }
    const auto* info = world.level();
    const auto roster = info != nullptr ? std::span<const LevelEnemy>{info->enemies}
                                        : std::span<const LevelEnemy>{};
    const auto gargoyle = std::ranges::find(roster, kGargoyleEnemyKind, &LevelEnemy::kind);
    const std::string_view form = gargoyle != roster.end() ? gargoyle->form : std::string_view{};
    const char realm = level.name.empty() ? 'G' : level.name.front();
    const std::array creatureTextures{&world.textures()};
    enemies.open(device, root, nullptr, Enemies::kMost, {}, 1);
    critters.open(device, root, nullptr, {}, realm, creatureTextures, form);
    bosses.open(device, root, nullptr, {}, realm, creatureTextures);
    const std::array generatorTextures{&world.textures(), &world.items().textures,
                                       &world.realmItems().textures};
    const auto players = static_cast<s32>(
        std::ranges::count_if(context.owners, [](u8 owner) { return owner != 0; }));
    // Binding loads the authored bodies/species but does not breed an enemy.
    generators.bind(device, world.layout(), enemies, &world.collision(), {}, players, roster,
                    level.realmId, &world.items(), generatorTextures);
    if (!StageResources::preload(world, enemies, critters, bosses)) {
        return false;
    }
    // Optional private simulation owns its own native stocks/clocks. It never
    // advances the catalog used by replica drawing or makes asset loads during replay.
    if (!prediction.open(device, root, world, enemies, players, difficultyGain)) {
        log::warn("Private opponent replay unavailable for {}", level.name);
    }
    auto bossStocks = bosses.resources();
    auto* boss = bossStocks.empty() ? nullptr : bossStocks.front();
    rocks.bind(device, world.layout(), world.items());
    const std::array<TextureSet*, 3> arenaTextures{&world.textures(), &world.realmItems().textures,
                                                   boss != nullptr ? &boss->archive.textures
                                                                   : nullptr};
    rocks.bindAnimations(device, world.items(), arenaTextures);
    const auto fixtureSources = StageResources::fixtureArchives(world, enemies, critters, bosses);
    FighterResources fighters;
    if (!pickups.bind(device, world.placedItems().archives()) ||
        !fixtures.bind(device, fixtureSources, generators, rocks) ||
        !fighters.bind(critters, bosses) ||
        !view.begin(context, world, enemies, projectiles, pickups, fixtures, std::move(fighters))) {
        return false;
    }
    std::array<PlayerFigure*, InputCommand::kSeats> figures{};
    classes.load(root / "pdata");
    for (usize seat = 0; seat < party.size(); ++seat) {
        const auto& profile = party[seat];
        if (!profile) {
            continue;
        }
        auto figure = PlayerFigure::load(device, root, profile->gameplayCopy(), false);
        if (!figure) {
            return false;
        }
        figures[seat] = figure.get();
        characters[seat] = profile->gameplayCopy().character;
        const auto character = profile->gameplayCopy();
        const f32 scale =
            PlayerFigure::bodyScale(character, PowerupEffects::of(character.progress().inventory));
        if (!view.setPlayer(static_cast<u8>(seat), std::move(figure), scale,
                            classes.stats(profile->gameplayCopy().character))) {
            return false;
        }
    }
    const std::array<TextureSet*, 5> lenders{&weapons.textures, &world.items().textures,
                                             &world.realmItems().textures,
                                             &world.powerups().textures, &common};
    for (usize seat = 0; seat < figures.size(); ++seat) {
        const auto character = characters[seat];
        const auto* stats = character ? classes.stats(*character) : nullptr;
        if (figures[seat] != nullptr && stats != nullptr) {
            // Unsupported effect rows stay authoritative. A failed optional
            // catalog must not prevent the ordinary stage from loading.
            turboEffects[seat].bind(device, *character, *stats, *figures[seat], weapons, lenders);
        }
    }
    auto stocks = critters.resources();
    stocks.insert(stocks.end(), bossStocks.begin(), bossStocks.end());
    const std::array archives{&world.items(), &world.realmItems(), &world.powerups()};
    if (!projectiles.addPlayers(device, weapons, figures, lenders) ||
        !projectiles.addEnemies(device, enemies, lenders) ||
        !projectiles.addStage(device, archives, stocks, lenders) ||
        !view.bindCompanions(device, world.powerups(), &weapons)) {
        return false;
    }
    view.loadPortalSkin(device, weapons.textures);
    view.loadDeathSkins(device, weapons);
    std::vector<HealthMeterReading> meters;
    if (boss != nullptr) {
        const auto append = [&](const CritterData& data) {
            if (data.meter().shown) {
                // Only the authored shape is needed; health comes from snapshots.
                meters.push_back({data.meter(), 1, 1});
            }
        };
        append(boss->data);
        for (const auto& child : boss->children) {
            append(child);
        }
    }
    const HudResources hud{&world.items().textures,
                           boss != nullptr ? &boss->archive.textures : nullptr, meters,
                           info != nullptr ? info->title : std::string_view{}, &world.powerups()};
    if (const auto index = world.powerups().textures.find("AAAWHITE")) {
        flash = &world.powerups().textures.texture(device, *index);
    }
    return view.loadHud(device, root, strings, hud);
}
bool ReplicaStage::open(RenderDevice& device, const std::filesystem::path& root,
                        const LevelRef& level, const MatchContext& context,
                        const PartyBootstrap::Party& party, const StringTable* strings,
                        f32 difficultyGain) {
    if (!context.valid() || context.epoch <= m_epoch ||
        context.transition == MatchTransition::Resume || (level.isTower() && level.name != "L1")) {
        return false;
    }
    for (usize seat = 0; seat < party.size(); ++seat) {
        if (party[seat].has_value() != (context.owners[seat] != 0) ||
            (party[seat] && !party[seat]->valid())) {
            return false;
        }
    }
    try {
        auto next = std::make_unique<Assets>();
        if (!next->load(device, root, level, context, party, strings, difficultyGain)) {
            return false;
        }
        m_assets = std::move(next);
        m_epoch = context.epoch;
        return true;
    } catch (const std::exception& error) {
        log::warn("Netplay stage load failed: {}", error.what());
        return false;
    }
}
bool ReplicaStage::resume(const MatchContext& context) {
    if (m_assets == nullptr || !m_assets->view.resume(context)) {
        return false;
    }
    m_epoch = context.epoch;
    return true;
}
void ReplicaStage::clear() {
    m_assets.reset();
    m_epoch = 0;
}
bool ReplicaStage::show(const CombatSnapshot& snapshot, const EnemyPresentation* presentation) {
    return m_assets != nullptr && m_assets->view.show(snapshot, presentation);
}
void ReplicaStage::draw(RenderDevice& device, const Mat4& frameProjection, f32 width, f32 height,
                        f32 textureFrame, const GameConfig& video,
                        const LocalTurboEffectPrediction* effects, f32 effectBlend) {
    if (m_assets != nullptr) {
        m_assets->view.draw(device, frameProjection, width, height, textureFrame, m_assets->flash,
                            nullptr, video, effects, effectBlend);
    }
}
const ReplicaView* ReplicaStage::view() const {
    return m_assets != nullptr ? &m_assets->view : nullptr;
}
const LevelWorld* ReplicaStage::world() const {
    return m_assets != nullptr ? &m_assets->world : nullptr;
}
const ProjectileResources* ReplicaStage::projectiles() const {
    return m_assets != nullptr ? &m_assets->projectiles : nullptr;
}
s32 ReplicaStage::character(u8 seat) const {
    return m_assets != nullptr && seat < m_assets->characters.size()
               ? m_assets->characters[seat].value_or(-1)
               : -1;
}
const ClassStats* ReplicaStage::classStats(u8 seat) const {
    const s32 known = character(seat);
    return known >= 0 ? m_assets->classes.stats(known) : nullptr;
}
const TurboEffectResources* ReplicaStage::turboEffects(u8 seat) const {
    return m_assets != nullptr && seat < m_assets->turboEffects.size() &&
                   m_assets->turboEffects[seat].stats() != nullptr
               ? &m_assets->turboEffects[seat]
               : nullptr;
}
const ItemArchive* ReplicaStage::weapons() const {
    return m_assets != nullptr ? &m_assets->weapons : nullptr;
}
OpponentPrediction* ReplicaStage::opponentPrediction() {
    return m_assets != nullptr && m_assets->prediction.loaded() ? &m_assets->prediction : nullptr;
}
} // namespace gdl::game
