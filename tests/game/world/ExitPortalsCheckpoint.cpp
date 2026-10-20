#include "game/world/ExitPortalsCheckpoint.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace gdl::game {
namespace {
using fixture_checkpoint::matrix;
using fixture_checkpoint::scalar;
using fixture_checkpoint::vector;

bool levelValid(const LevelRef& level) {
    const auto text = [](const std::string& value, usize limit) {
        return value.size() <= limit && value.find('\0') == std::string::npos;
    };
    return text(level.realm, 64) && text(level.name, 64) && text(level.title, 256) &&
           text(level.directory, 512) && text(level.items, 512) && text(level.ownItems, 512) &&
           level.realmId >= -1 && level.realmId <= 255 && level.index >= 0 && level.index <= 65535;
}
bool sameIdentity(const ExitPortalsCheckpoint::Portal& saved, const ExitPortals::Portal& live,
                  s32 archive) {
    return saved.source.index == static_cast<usize>(live.instance) && saved.secret == live.secret &&
           saved.minPlayers == live.minPlayers && saved.radius == live.radius &&
           saved.shut == live.shut && saved.archive == archive &&
           saved.model == live.model.bound() && saved.tag == live.tag &&
           saved.destination == live.destination && saved.support == live.support &&
           saved.supportLocal == live.supportLocal && saved.pose.has_value() == live.pose.posed();
}
} // namespace

bool ExitPortalsCheckpoint::State::valid() const {
    if (portals.size() > kMaxPortals || waiting.size() > kMaxWaiting || visitors.size() > 4 ||
        !fixture_checkpoint::name(tree)) {
        return false;
    }
    for (const s32 sequence : sequences) {
        if (sequence < -1 || sequence > 65535 || (tree.empty() && sequence != -1)) {
            return false;
        }
    }
    std::array<bool, 4> seen{};
    for (const auto& visitor : visitors) {
        if (visitor.party < 0 || visitor.party >= 4 || seen[static_cast<usize>(visitor.party)] ||
            visitor.ticks < 0 || visitor.ticks > ExitPortals::kIdleTicks) {
            return false;
        }
        seen[static_cast<usize>(visitor.party)] = true;
    }
    if (!std::ranges::all_of(waiting, [](s32 party) { return party >= 0 && party < 4; })) {
        return false;
    }
    usize triangles = 0;
    usize particles = 0;
    std::optional<usize> previous;
    for (const auto& portal : portals) {
        const auto& source = portal.source;
        if (!source.valid() || source.info.type != ExitPortals::kExitItem ||
            (previous && source.index <= *previous) ||
            source.instance.collision.size() > kMaxTriangles - triangles ||
            portal.secret != (source.info.subtype == ExitPortals::kSecretSubtype) ||
            portal.minPlayers != source.instance.minPlayers ||
            portal.radius != source.info.radius || !vector(portal.position) ||
            !matrix(portal.transform) || Vec3{portal.transform[3]} != portal.position ||
            !matrix(portal.supportLocal) || portal.support < -1 || portal.support > 65535 ||
            (!collision && portal.support >= 0) || !scalar(portal.radius) || portal.radius < 0 ||
            !scalar(portal.alpha) || portal.alpha < 0 || portal.alpha > 1 ||
            (portal.departurePosition && !vector(*portal.departurePosition)) ||
            portal.archive < -1 || portal.archive > 1 ||
            portal.tag != ExitPortals::tagOf(source.instance) ||
            (portal.destination && !levelValid(*portal.destination)) || portal.action < 0 ||
            portal.action > ExitPortals::kLast || portal.ticksLeft < 0 ||
            portal.ticksLeft > 1'000'000 || !portal.player.valid() ||
            (portal.player.playing && tree.empty()) || !portal.icon.valid() ||
            (portal.pose && (!portal.pose->valid() || portal.pose->tree != tree)) ||
            (portal.secret && (portal.model || portal.archive != -1 || portal.shut))) {
            return false;
        }
        triangles += source.instance.collision.size();
        for (const auto& entry : portal.icon.state.particles.field.entries) {
            if (entry.emitter.particles.size() > kMaxParticles - particles) {
                return false;
            }
            particles += entry.emitter.particles.size();
        }
        previous = source.index;
    }
    return true;
}

struct ExitPortalsCheckpoint::Resources {
    struct Row {
        fixture_checkpoint::Placement source;
        const ExitPortals::Portal* address = nullptr;
        const ItemArchive* archive = nullptr;
        const ItemArchive* iconArchive = nullptr;
        const TreeInfo* iconTree = nullptr;
        bool model = false, secret = false, shut = false;
        std::string tag;
        std::optional<LevelRef> destination;
    };
    std::vector<Row> rows;
    const TreeInfo* tree = nullptr;
    const WorldCollision* collision = nullptr;
    std::array<s32, ExitPortals::kSequences.size()> sequences{};
    std::shared_ptr<const u8> binding;
};
struct ExitPortalsCheckpoint::Prepared::Data {
    const ExitPortalsCheckpoint* wrapper = nullptr;
    ExitPortals* owner = nullptr;
    std::shared_ptr<const u8> lifetime, binding;
    u64 generation = 0;
    std::vector<ExitPortals::Portal> portals;
    std::vector<s32> waiting;
    std::vector<ExitPortals::VisitorWait> visitors;
};
ExitPortalsCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
ExitPortalsCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
ExitPortalsCheckpoint::Prepared&
ExitPortalsCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
ExitPortalsCheckpoint::Prepared::~Prepared() = default;
ExitPortalsCheckpoint::ExitPortalsCheckpoint(ExitPortals& owner, const WorldLayout& layout,
                                             ItemArchive& items, ItemArchive* realm)
    : m_owner(owner), m_layout(layout), m_items(items), m_realm(realm) {}
ExitPortalsCheckpoint::~ExitPortalsCheckpoint() = default;

s32 ExitPortalsCheckpoint::archiveId(const ItemArchive* archive) const {
    if (!archive) {
        return -1;
    }
    if (archive == &m_items) {
        return 0;
    }
    return archive == m_realm ? 1 : -2;
}
bool ExitPortalsCheckpoint::bound() const {
    if (!m_resources || m_resources->binding != m_owner.m_checkpointBinding ||
        m_resources->tree != m_owner.m_tree || m_resources->collision != m_owner.m_collision ||
        m_resources->sequences != m_owner.m_sequences ||
        m_resources->rows.size() != m_owner.m_portals.size()) {
        return false;
    }
    for (usize index = 0; index < m_resources->rows.size(); ++index) {
        const auto& row = m_resources->rows[index];
        const auto& live = m_owner.m_portals[index];
        const auto icon = live.icon.presentation();
        if (row.address != &live || !row.source.matches(m_layout) ||
            row.source.index != static_cast<usize>(live.instance) || row.archive != live.archive ||
            row.iconArchive != icon.archive || row.iconTree != icon.tree ||
            row.model != live.model.bound() || row.secret != live.secret || row.shut != live.shut ||
            row.tag != live.tag || row.destination != live.destination) {
            return false;
        }
    }
    return true;
}
bool ExitPortalsCheckpoint::bindResources() {
    if (m_owner.size() > State::kMaxPortals) {
        return false;
    }
    auto resources = std::make_unique<Resources>();
    resources->binding = m_owner.m_checkpointBinding;
    resources->tree = m_owner.m_tree;
    resources->collision = m_owner.m_collision;
    resources->sequences = m_owner.m_sequences;
    for (const auto& live : m_owner.m_portals) {
        if (live.instance < 0 || archiveId(live.archive) < -1) {
            return false;
        }
        const auto source =
            fixture_checkpoint::placement(&m_layout, static_cast<usize>(live.instance));
        const auto icon = live.icon.presentation();
        if (!source || !source->valid() || source->info.type != ExitPortals::kExitItem ||
            archiveId(icon.archive) < -1 || !ItemFigureCheckpoint::capture(live.icon)) {
            return false;
        }
        resources->rows.push_back({*source, &live, live.archive, icon.archive, icon.tree,
                                   live.model.bound(), live.secret, live.shut, live.tag,
                                   live.destination});
    }
    m_resources = std::move(resources);
    m_lifetime = std::make_shared<const u8>(0);
    return checkpoint().has_value();
}
bool ExitPortalsCheckpoint::binds(const ExitPortals& owner) const {
    return &owner == &m_owner && bound();
}
bool ExitPortalsCheckpoint::binds(const WorldLayout& layout, const ItemArchive& items,
                                  const ItemArchive* realm, const WorldCollision& collision) const {
    return bound() && &layout == &m_layout && &items == &m_items && realm == m_realm &&
           &collision == m_owner.m_collision;
}
std::optional<ExitPortalsCheckpoint::State> ExitPortalsCheckpoint::checkpoint() const {
    if (!bound()) {
        return std::nullopt;
    }
    State state;
    state.tree = m_owner.m_tree ? m_owner.m_tree->name : "";
    state.sequences = m_owner.m_sequences;
    state.collision = m_owner.m_collision != nullptr;
    state.waiting = m_owner.m_wait.waiting;
    for (const auto& visitor : m_owner.m_wait.visitors) {
        state.visitors.push_back({visitor.party, visitor.ticks});
    }
    for (usize index = 0; index < m_owner.size(); ++index) {
        const auto& live = m_owner.m_portals[index];
        const auto icon = ItemFigureCheckpoint::capture(live.icon);
        const auto pose = live.pose.checkpoint();
        if (!icon || (live.pose.posed() && !pose)) {
            return std::nullopt;
        }
        state.portals.push_back({m_resources->rows[index].source,
                                 live.position,
                                 live.transform,
                                 live.support,
                                 live.supportLocal,
                                 live.secret,
                                 live.minPlayers,
                                 live.radius,
                                 live.consumed,
                                 live.shut,
                                 live.alpha,
                                 live.departurePosition,
                                 archiveId(live.archive),
                                 live.model.bound(),
                                 *icon,
                                 live.tag,
                                 live.destination,
                                 live.action,
                                 live.ticksLeft,
                                 pose,
                                 live.player.checkpoint()});
    }
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}
std::optional<ExitPortalsCheckpoint::Prepared>
ExitPortalsCheckpoint::prepareRestore(const State& state) const {
    if (!bound() || !state.valid() || state.portals.size() != m_owner.size() ||
        state.tree != (m_owner.m_tree ? m_owner.m_tree->name : "") ||
        state.sequences != m_owner.m_sequences ||
        state.collision != (m_owner.m_collision != nullptr) ||
        m_owner.m_checkpointGeneration == std::numeric_limits<u64>::max()) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->wrapper = this;
    data->owner = &m_owner;
    data->lifetime = m_lifetime;
    data->binding = m_owner.m_checkpointBinding;
    data->generation = m_owner.m_checkpointGeneration;
    data->waiting = state.waiting;
    for (const auto& visitor : state.visitors) {
        data->visitors.push_back({visitor.party, visitor.ticks});
    }
    data->portals.reserve(state.portals.size());
    for (usize index = 0; index < state.portals.size(); ++index) {
        const auto& saved = state.portals[index];
        const auto& live = m_owner.m_portals[index];
        if (saved.source != m_resources->rows[index].source ||
            !sameIdentity(saved, live, archiveId(live.archive))) {
            return std::nullopt;
        }
        auto icon = ItemFigureCheckpoint::prepare(live.icon, saved.icon);
        if (!icon) {
            return std::nullopt;
        }
        const TreeSequenceInfo* sequence = nullptr;
        if (saved.player.playing) {
            if (!m_owner.m_tree || saved.player.sequence >= m_owner.m_tree->sequences.size()) {
                return std::nullopt;
            }
            sequence = &m_owner.m_tree->sequences[saved.player.sequence];
        }
        ExitPortals::Portal portal;
        if (!portal.player.restore(saved.player, sequence) ||
            (saved.pose &&
             (!m_owner.m_tree || !portal.pose.restore(*m_owner.m_tree, *saved.pose)))) {
            return std::nullopt;
        }
        portal.instance = live.instance;
        portal.secret = saved.secret;
        portal.minPlayers = saved.minPlayers;
        portal.position = saved.position;
        portal.transform = saved.transform;
        portal.support = saved.support;
        portal.supportLocal = saved.supportLocal;
        portal.radius = saved.radius;
        portal.consumed = saved.consumed;
        portal.shut = saved.shut;
        portal.alpha = saved.alpha;
        portal.departurePosition = saved.departurePosition;
        portal.archive = live.archive;
        portal.icon = std::move(*icon);
        portal.tag = saved.tag;
        portal.destination = saved.destination;
        portal.action = saved.action;
        portal.ticksLeft = saved.ticksLeft;
        portal.model = live.model;
        if (saved.player.playing) {
            portal.model.setFrame(saved.player.sequence, static_cast<s32>(saved.player.frame));
        }
        data->portals.push_back(std::move(portal));
    }
    return Prepared{std::move(data)};
}
bool ExitPortalsCheckpoint::canCommit(const Prepared& prepared) const {
    if (!bound() || !prepared.m_data) {
        return false;
    }
    const auto& data = *prepared.m_data;
    return data.wrapper == this && data.owner == &m_owner && data.lifetime == m_lifetime &&
           data.binding == m_owner.m_checkpointBinding &&
           data.generation == m_owner.m_checkpointGeneration &&
           data.portals.size() == m_owner.size();
}
bool ExitPortalsCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    for (usize index = 0; index < data->portals.size(); ++index) {
        m_owner.m_portals[index] = std::move(data->portals[index]);
    }
    m_owner.m_wait.waiting.swap(data->waiting);
    m_owner.m_wait.visitors.swap(data->visitors);
    ++m_owner.m_checkpointGeneration;
    return true;
}
bool ExitPortalsCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl::game
