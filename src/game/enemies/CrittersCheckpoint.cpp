#include "game/enemies/CrittersCheckpoint.h"

#include <algorithm>
#include <exception>
#include <tuple>
#include <utility>

#include "game/world/FixtureCheckpointDetail.h"

namespace gdl::game {
namespace {
auto bankKey(const CrittersCheckpoint::Bank& bank) {
    return std::tie(bank.kind, bank.name);
}
bool bankIdentity(const CrittersCheckpoint::Bank& a, const CrittersCheckpoint::Bank& b) {
    return std::tie(a.id, a.kind, a.name, a.tree) == std::tie(b.id, b.kind, b.name, b.tree);
}
} // namespace
bool CrittersCheckpoint::State::valid() const {
    if (banks.size() > kMaxStocks || realm < 'A' || realm > 'Z' ||
        !fixture_checkpoint::name(gargoyleForm) || !events.valid() || !events.arena.empty() ||
        !events.tramples.empty()) {
        return false;
    }
    for (usize i = 0; i < banks.size(); ++i) {
        const auto& bank = banks[i];
        if (bank.id != i + 1 || !fixture_checkpoint::name(bank.name) || bank.name.empty() ||
            !fixture_checkpoint::name(bank.tree) || bank.tree.empty() || !bank.clock.valid() ||
            (bank.kind != CombatantKind::Golem && bank.kind != CombatantKind::General &&
             bank.kind != CombatantKind::Gargoyle) ||
            (i > 0 && bankKey(banks[i - 1]) >= bankKey(bank))) {
            return false;
        }
    }
    for (usize index = 0; index < actors.size(); ++index) {
        const auto& actor = actors[index];
        if (!actor.valid() || actor.lookouts != std::optional{lookouts} ||
            actor.parts.size() != 1 ||
            (actor.parts[0].id != static_cast<s32>(index) &&
             (actor.parts[0].actor.definition.resource != 0 || actor.parts[0].id != -1)) ||
            actor.parts[0].actor.definition.resource > banks.size()) {
            return false;
        }
    }
    // The immutable population tuning is independently validated; unused slots
    // legitimately retain prior native tuning through clear(), so do not equate them.
    CombatantCheckpoint::Part tuning;
    tuning.scales = scales;
    return tuning.valid();
}
struct CrittersCheckpoint::Prepared::Data {
    const CrittersCheckpoint* wrapper = nullptr;
    const Critters* owner = nullptr;
    std::shared_ptr<const u8> binding, lifetime;
    std::vector<CombatantCheckpoint::Prepared> actors;
    std::vector<TextureAnimator::Checkpoint> banks;
    CombatantCheckpoint::Events events;
    std::vector<CombatShot> shots;
};
CrittersCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
CrittersCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
CrittersCheckpoint::Prepared&
CrittersCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
CrittersCheckpoint::Prepared::~Prepared() = default;

CrittersCheckpoint::CrittersCheckpoint(Critters& owner)
    : m_owner(owner), m_binding(owner.m_checkpointBinding), m_device(owner.m_device),
      m_collision(owner.m_collision), m_hazards(owner.m_hazards), m_hitFlash(owner.m_hitFlash),
      m_scales(owner.m_scales), m_realm(owner.m_realm), m_gargoyleForm(owner.m_gargoyleForm),
      m_lookouts(owner.m_lookouts) {
    if (owner.m_stocks.size() > kMaxStocks) {
        return;
    }
    std::vector<CombatantAssets*> stocks;
    for (const auto& stock : owner.m_stocks) {
        if (!stock || !stock->tree) {
            return;
        }
        stocks.push_back(stock.get());
    }
    std::ranges::sort(stocks, {}, [](const auto* stock) {
        return std::tie(stock->definition.kind, stock->definition.name);
    });
    for (usize i = 0; i < stocks.size(); ++i) {
        auto* stock = stocks[i];
        m_resources.push_back({static_cast<u32>(i + 1), stock});
        m_banks.push_back({static_cast<u32>(i + 1),
                           stock->definition.kind,
                           stock->definition.name,
                           stock->tree->name,
                           {}});
    }
    for (usize i = 0; i < m_actors.size(); ++i) {
        m_actors[i] = std::make_unique<CombatantCheckpoint>(
            owner.m_critters[i], m_resources,
            CombatantCheckpoint::Bindings{m_collision, m_hazards, &owner.m_lookouts});
    }
}
CrittersCheckpoint::~CrittersCheckpoint() = default;
bool CrittersCheckpoint::bound() const {
    // Check the native token before dereferencing any borrowed stock: close()
    // destroys those objects and stockFor() can replace the trusted catalog.
    if (m_binding != m_owner.m_checkpointBinding || m_device != m_owner.m_device ||
        m_collision != m_owner.m_collision || m_hazards != m_owner.m_hazards ||
        m_hitFlash != m_owner.m_hitFlash || m_resources.size() != m_owner.m_stocks.size() ||
        m_scales != m_owner.m_scales || m_realm != m_owner.m_realm ||
        m_gargoyleForm != m_owner.m_gargoyleForm || m_lookouts != m_owner.m_lookouts) {
        return false;
    }
    return std::ranges::all_of(m_actors, [](const auto& owner) { return owner != nullptr; }) &&
           std::ranges::all_of(m_resources, [](const auto& r) {
               return r.stock && r.stock->tree && r.stock->archive.loaded();
           });
}
bool CrittersCheckpoint::matches(const State& state) const {
    return bound() && state.scales == m_scales && state.realm == m_realm &&
           state.gargoyleForm == m_gargoyleForm && state.lookouts == m_lookouts &&
           state.banks.size() == m_banks.size() &&
           std::equal(state.banks.begin(), state.banks.end(), m_banks.begin(), bankIdentity);
}
std::optional<CrittersCheckpoint::State> CrittersCheckpoint::checkpoint() const {
    if (!bound()) {
        return std::nullopt;
    }
    State out;
    out.scales = m_scales;
    out.realm = m_realm;
    out.gargoyleForm = m_gargoyleForm;
    out.lookouts = m_lookouts;
    out.banks = m_banks;
    for (usize i = 0; i < m_resources.size(); ++i) {
        out.banks[i].clock = m_resources[i].stock->textures.checkpoint();
    }
    for (usize i = 0; i < m_actors.size(); ++i) {
        auto saved = m_actors[i]->checkpoint();
        if (!saved) {
            return std::nullopt;
        }
        out.actors[i] = std::move(*saved);
    }
    auto events = m_actors.front()->captureEvents(
        m_owner.m_cues, m_owner.m_grabs, m_owner.m_spews, m_owner.m_shots, {}, {}, m_owner.m_rams,
        m_owner.m_pushes, m_owner.m_blows, m_owner.m_losses);
    if (!events) {
        return std::nullopt;
    }
    out.events = std::move(*events);
    return out.valid() ? std::optional{std::move(out)} : std::nullopt;
}
std::optional<CrittersCheckpoint::Prepared>
CrittersCheckpoint::prepareRestore(const State& state) const {
    if (!state.valid() || !matches(state)) {
        return std::nullopt;
    }
    auto prepared = std::make_unique<Prepared::Data>();
    prepared->wrapper = this;
    prepared->owner = &m_owner;
    prepared->binding = m_binding;
    prepared->lifetime = m_lifetime;
    for (usize i = 0; i < m_actors.size(); ++i) {
        auto actor = m_actors[i]->prepareRestore(state.actors[i]);
        if (!actor) {
            return std::nullopt;
        }
        prepared->actors.push_back(std::move(*actor));
    }
    for (const auto& bank : state.banks) {
        prepared->banks.push_back(bank.clock);
    }
    prepared->events = state.events;
    auto shots = m_actors.front()->restoreShots(state.events.shots);
    if (!shots) {
        return std::nullopt;
    }
    prepared->shots = std::move(*shots);
    Prepared out{std::move(prepared)};
    return canCommit(out) ? std::optional{std::move(out)} : std::nullopt;
}
bool CrittersCheckpoint::canCommit(const Prepared& value) const {
    if (!value.m_data || !bound()) {
        return false;
    }
    const auto& prepared = *value.m_data;
    if (prepared.wrapper != this || prepared.owner != &m_owner || prepared.binding != m_binding ||
        prepared.lifetime != m_lifetime || prepared.actors.size() != m_actors.size() ||
        prepared.banks.size() != m_resources.size()) {
        return false;
    }
    for (usize i = 0; i < m_actors.size(); ++i) {
        if (!m_actors[i]->canCommit(prepared.actors[i])) {
            return false;
        }
    }
    return std::ranges::all_of(prepared.banks, &TextureAnimator::Checkpoint::valid);
}
bool CrittersCheckpoint::commitRestore(Prepared&& value) {
    if (!canCommit(value)) {
        return false;
    }
    auto prepared = std::move(value.m_data);
    for (usize i = 0; i < m_actors.size(); ++i) {
        if (!m_actors[i]->commitRestore(std::move(prepared->actors[i]))) {
            std::terminate();
        }
    }
    for (usize i = 0; i < m_resources.size(); ++i) {
        if (!m_resources[i].stock->textures.restore(prepared->banks[i])) {
            std::terminate();
        }
    }
    m_owner.m_cues.swap(prepared->events.cues);
    m_owner.m_grabs.swap(prepared->events.grabs);
    m_owner.m_spews.swap(prepared->events.spews);
    m_owner.m_shots.swap(prepared->shots);
    m_owner.m_rams.swap(prepared->events.rams);
    m_owner.m_pushes.swap(prepared->events.pushes);
    m_owner.m_blows.swap(prepared->events.blows);
    m_owner.m_losses.swap(prepared->events.losses);
    return true;
}
bool CrittersCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl::game
