#include "game/world/TrapsCheckpoint.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <tuple>
#include <utility>

namespace gdl::game {
namespace {
bool scalar(f32 value) {
    return std::isfinite(value) && std::abs(value) <= 1'000'000;
}
bool vector(const Vec3& value) {
    return scalar(value.x) && scalar(value.y) && scalar(value.z);
}
bool matrix(const Mat4& value) {
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            if (!scalar(value[column][row])) {
                return false;
            }
        }
    }
    return value[0].w == 0 && value[1].w == 0 && value[2].w == 0 && value[3].w == 1;
}
auto info(const ItemInfo& v) {
    return std::tie(v.type, v.subtype, v.name, v.radius, v.height, v.xSize, v.zSize,
                    v.collisionType, v.collisionFlags, v.collisionOffset, v.objectFlags,
                    v.properties, v.value, v.armor, v.hitPoints, v.activeType, v.activeOff,
                    v.activeOn, v.choices);
}
bool samePlacement(const ItemInstance& a, const ItemInstance& b) {
    return std::tie(a.info, a.minPlayers, a.flags, a.name, a.position, a.rotation, a.params) ==
               std::tie(b.info, b.minPlayers, b.flags, b.name, b.position, b.rotation, b.params) &&
           a.collision.size() == b.collision.size() &&
           std::ranges::equal(a.collision, b.collision, [](const auto& x, const auto& y) {
               return x.normal == y.normal && x.vertices == y.vertices;
           });
}
auto shape(const Obstacle& v) {
    return std::tie(v.centre, v.yaw, v.halfAcross, v.halfAlong, v.height, v.solid,
                    v.cylinderRadius);
}
bool sameShape(const Obstacle& a, const Obstacle& b) {
    return shape(a) == shape(b) && a.enemyItem.has_value() == b.enemyItem.has_value() &&
           (!a.enemyItem || (a.enemyItem->cylindrical == b.enemyItem->cylindrical &&
                             a.enemyItem->radius == b.enemyItem->radius));
}
bool name(const std::string& value) {
    return value.size() <= 64 && value.find('\0') == std::string::npos;
}
auto scalars(const TrapsCheckpoint::Trap& v) {
    return std::tie(v.instance, v.damage, v.offTime, v.subtype, v.properties, v.action, v.ticksLeft,
                    v.minPlayers, v.shown, v.disarmed, v.gone, v.rectangularContact, v.figure,
                    v.support);
}
u32 randomState(const std::minstd_rand& random) {
    // The standard engine's stream format is its current integer state. This
    // avoids peeking into vendor-specific engine memory or advancing the RNG.
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << random;
    const auto text = out.str();
    u32 state = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): the end pointer of a contiguous buffer
    const auto result = std::from_chars(text.data(), text.data() + text.size(), state);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): the end pointer of a contiguous buffer
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? state : 0;
}
} // namespace
bool TrapsCheckpoint::Trap::operator==(const Trap& other) const {
    return scalars(*this) == scalars(other) && info(definition) == info(other.definition) &&
           samePlacement(placement, other.placement) && sameShape(box, other.box);
}
bool TrapsCheckpoint::State::valid() const {
    if (traps.size() > kMaxTraps || wakes.size() > traps.size() || gaps.size() > 4 ||
        random < std::minstd_rand::min() || random > std::minstd_rand::max() ||
        !scalar(timeScale) || timeScale < 0 || (!layout && !traps.empty()) ||
        !std::ranges::all_of(gaps, [](f32 gap) { return scalar(gap) && gap >= 0; })) {
        return false;
    }
    s32 previous = -1;
    usize triangles = 0;
    usize choices = 0;
    for (const auto& trap : traps) {
        const auto& def = trap.definition;
        const auto& source = trap.placement;
        const auto& box = trap.box;
        const auto& support = trap.support;
        if (trap.instance <= previous || trap.instance > 65535 || def.type != ItemInfo::kTrap ||
            !name(def.name) || !name(source.name) || source.info < 0 || source.info > 65535 ||
            def.choices.size() > 65535 - choices || source.collision.size() > 65535 - triangles ||
            !scalar(def.radius) || !scalar(def.height) || !scalar(def.xSize) ||
            !scalar(def.zSize) || !vector(def.collisionOffset) || !vector(source.position) ||
            !vector(source.rotation) || !scalar(trap.damage) ||
            trap.minPlayers != source.minPlayers || trap.minPlayers < 0 || trap.minPlayers > 255 ||
            trap.ticksLeft < 0 || trap.ticksLeft > std::numeric_limits<s32>::max() - 120 ||
            (trap.disarmed && trap.action != Traps::kResting) || trap.action < 0 ||
            trap.action > 65535 || !trap.figure.valid() || !vector(box.centre) ||
            !scalar(box.yaw) || !scalar(box.halfAcross) || box.halfAcross < 0 ||
            !scalar(box.halfAlong) || box.halfAlong < 0 || !scalar(box.height) || box.height < 0 ||
            !scalar(box.cylinderRadius) || box.cylinderRadius < 0 ||
            (box.enemyItem && (!scalar(box.enemyItem->radius) || box.enemyItem->radius < 0)) ||
            support.object < -1 || support.object > 65535 || !matrix(support.local) ||
            !vector(support.boxOffset) || !scalar(support.boxYaw) ||
            (!collision && support.object >= 0) || (trap.gone && (!trap.disarmed || trap.shown))) {
            return false;
        }
        triangles += source.collision.size();
        choices += def.choices.size();
        previous = trap.instance;
        for (const auto& triangle : source.collision) {
            if (!vector(triangle.normal) || !std::ranges::all_of(triangle.vertices, vector)) {
                return false;
            }
        }
    }
    for (usize i = 0; i < wakes.size(); ++i) {
        if (wakes[i] >= traps.size() || (i != 0 && wakes[i] <= wakes[i - 1])) {
            return false;
        }
    }
    return true;
}
struct TrapsCheckpoint::Resources {
    const WorldLayout* layout = nullptr;
    AssetScope scope;
    const WorldCollision* collision = nullptr;
    f32 timeScale = 1;
    std::shared_ptr<const u8> binding;
    struct Row {
        s32 instance = -1;
        ItemInfo definition;
        ItemInstance placement;
        f32 damage = 0;
        s32 offTime = 0;
        std::unique_ptr<ItemFigure> normal, disarmed;
    };
    std::vector<Row> rows;
};
struct TrapsCheckpoint::Prepared::Data {
    const TrapsCheckpoint* wrapper = nullptr;
    Traps* owner = nullptr;
    std::shared_ptr<const u8> lifetime, binding;
    u64 generation = 0;
    std::vector<const Traps::Trap*> roster;
    std::vector<Traps::Trap> traps;
    std::vector<usize> wakes;
    std::vector<f32> gaps;
    std::minstd_rand random;
    f32 timeScale = 1;
};
TrapsCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
TrapsCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
TrapsCheckpoint::Prepared& TrapsCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
TrapsCheckpoint::Prepared::~Prepared() = default;
TrapsCheckpoint::TrapsCheckpoint(Traps& owner) : m_owner(owner) {}
TrapsCheckpoint::~TrapsCheckpoint() = default;
bool TrapsCheckpoint::bound() const {
    return m_resources && m_resources->binding == m_owner.m_checkpointBinding &&
           m_resources->layout == m_owner.m_layout &&
           std::ranges::equal(m_resources->scope.archives(), m_owner.m_scope.archives()) &&
           m_resources->collision == m_owner.m_collision &&
           m_resources->rows.size() == m_owner.m_sim.traps.size();
}
bool TrapsCheckpoint::bindResources(RenderDevice& device) {
    auto resources = std::make_unique<Resources>();
    resources->layout = m_owner.m_layout;
    resources->scope = m_owner.m_scope;
    resources->collision = m_owner.m_collision;
    resources->timeScale = m_owner.m_sim.timeScale;
    resources->binding = m_owner.m_checkpointBinding;
    if (m_owner.m_sim.traps.size() > State::kMaxTraps ||
        (!resources->layout && !m_owner.m_sim.traps.empty())) {
        return false;
    }
    Traps prototypes;
    if (resources->layout) {
        prototypes.bind(device, *resources->layout, resources->scope, resources->collision, 1, 1,
                        1);
    }
    if (prototypes.size() != m_owner.size()) {
        return false;
    }
    for (usize i = 0; i < m_owner.size(); ++i) {
        const auto& source = m_owner.m_sim.traps[i];
        auto& prototype = prototypes.m_sim.traps[i];
        if (source.instance != prototype.instance || source.instance < 0 ||
            static_cast<usize>(source.instance) >= resources->layout->itemInstances().size()) {
            return false;
        }
        const auto& instance =
            resources->layout->itemInstances()[static_cast<usize>(source.instance)];
        if (instance.info < 0 ||
            static_cast<usize>(instance.info) >= resources->layout->itemInfos().size()) {
            return false;
        }
        const auto normal = ItemFigureCheckpoint::capture(prototype.figure);
        if (!normal) {
            return false;
        }
        auto normalFigure = ItemFigureCheckpoint::prepare(prototype.figure, *normal);
        if (!normalFigure || !prototypes.disarm(i, device, *resources->layout, resources->scope,
                                                resources->collision)) {
            return false;
        }
        const auto disarmed = ItemFigureCheckpoint::capture(prototype.figure);
        if (!disarmed) {
            return false;
        }
        auto disarmedFigure = ItemFigureCheckpoint::prepare(prototype.figure, *disarmed);
        if (!disarmedFigure) {
            return false;
        }
        resources->rows.push_back(
            {source.instance, resources->layout->itemInfos()[static_cast<usize>(instance.info)],
             instance, source.damage, source.offTime, std::move(normalFigure),
             std::move(disarmedFigure)});
    }
    m_resources = std::move(resources);
    m_lifetime = std::make_shared<const u8>(0);
    return true;
}
std::optional<TrapsCheckpoint::State> TrapsCheckpoint::checkpoint() const {
    if (!bound()) {
        return std::nullopt;
    }
    State state;
    state.random = randomState(m_owner.m_sim.random);
    state.timeScale = m_owner.m_sim.timeScale;
    state.layout = m_owner.m_layout != nullptr;
    state.collision = m_owner.m_collision != nullptr;
    state.wakes = m_owner.m_sim.wakes;
    state.gaps = m_owner.m_sim.gaps;
    for (usize i = 0; i < m_owner.size(); ++i) {
        const auto& source = m_owner.m_sim.traps[i];
        const auto& resource = m_resources->rows[i];
        auto figure = ItemFigureCheckpoint::capture(source.figure);
        if (!figure || resource.instance != source.instance) {
            return std::nullopt;
        }
        const auto& support = source.support;
        state.traps.push_back(
            {source.instance,
             resource.definition,
             resource.placement,
             source.damage,
             source.offTime,
             source.subtype,
             source.properties,
             source.action,
             source.ticksLeft,
             source.minPlayers,
             source.shown,
             source.disarmed,
             source.gone,
             source.rectangularContact,
             std::move(*figure),
             source.box,
             {support.m_object, support.m_local, support.m_boxOffset, support.m_boxYaw}});
    }
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}
std::optional<TrapsCheckpoint::Prepared> TrapsCheckpoint::prepareRestore(const State& state) const {
    if (!bound() || !state.valid() || state.traps.size() != m_owner.size() ||
        state.layout != (m_owner.m_layout != nullptr) ||
        state.collision != (m_owner.m_collision != nullptr) ||
        state.timeScale != m_resources->timeScale ||
        m_owner.m_checkpointGeneration == std::numeric_limits<u64>::max()) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->wrapper = this;
    data->owner = &m_owner;
    data->lifetime = m_lifetime;
    data->binding = m_owner.m_checkpointBinding;
    data->generation = m_owner.m_checkpointGeneration;
    data->wakes = state.wakes;
    data->gaps = state.gaps;
    data->random.seed(state.random);
    data->timeScale = state.timeScale;
    for (usize i = 0; i < state.traps.size(); ++i) {
        const auto& saved = state.traps[i];
        const auto& resource = m_resources->rows[i];
        if (saved.instance != resource.instance ||
            info(saved.definition) != info(resource.definition) ||
            !samePlacement(saved.placement, resource.placement) ||
            saved.damage != resource.damage || saved.offTime != resource.offTime ||
            saved.subtype != resource.definition.subtype ||
            saved.properties != resource.definition.properties ||
            saved.rectangularContact != (resource.definition.collisionType == 3)) {
            return std::nullopt;
        }
        const auto& prototype = saved.disarmed ? resource.disarmed : resource.normal;
        if (static_cast<usize>(saved.action) >= std::max<usize>(prototype->sequenceCount(), 3)) {
            return std::nullopt;
        }
        auto figure = ItemFigureCheckpoint::prepare(*prototype, saved.figure);
        if (!figure) {
            return std::nullopt;
        }
        Traps::Trap trap;
        trap.instance = saved.instance;
        trap.damage = saved.damage;
        trap.offTime = saved.offTime;
        trap.subtype = saved.subtype;
        trap.name = resource.definition.name;
        trap.properties = saved.properties;
        trap.action = saved.action;
        trap.ticksLeft = saved.ticksLeft;
        trap.minPlayers = saved.minPlayers;
        trap.shown = saved.shown;
        trap.disarmed = saved.disarmed;
        trap.gone = saved.gone;
        trap.rectangularContact = saved.rectangularContact;
        trap.figure = std::move(*figure);
        trap.box = saved.box;
        trap.support.m_object = saved.support.object;
        trap.support.m_local = saved.support.local;
        trap.support.m_boxOffset = saved.support.boxOffset;
        trap.support.m_boxYaw = saved.support.boxYaw;
        data->roster.push_back(&m_owner.m_sim.traps[i]);
        data->traps.push_back(std::move(trap));
    }
    return Prepared{std::move(data)};
}
bool TrapsCheckpoint::canCommit(const Prepared& prepared) const {
    if (!bound() || !prepared.m_data) {
        return false;
    }
    const auto& data = *prepared.m_data;
    if (data.wrapper != this || data.owner != &m_owner || data.lifetime != m_lifetime ||
        data.binding != m_owner.m_checkpointBinding ||
        data.generation != m_owner.m_checkpointGeneration || data.roster.size() != m_owner.size()) {
        return false;
    }
    for (usize i = 0; i < data.roster.size(); ++i) {
        if (data.roster[i] != &m_owner.m_sim.traps[i]) {
            return false;
        }
    }
    return true;
}
bool TrapsCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    for (usize i = 0; i < data->traps.size(); ++i) {
        m_owner.m_sim.traps[i] = std::move(data->traps[i]);
    }
    m_owner.m_sim.wakes.swap(data->wakes);
    m_owner.m_sim.gaps.swap(data->gaps);
    m_owner.m_sim.random = data->random;
    m_owner.m_sim.timeScale = data->timeScale;
    ++m_owner.m_checkpointGeneration;
    return true;
}
bool TrapsCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl::game
