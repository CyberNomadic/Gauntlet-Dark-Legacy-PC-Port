#include "game/world/BreakablesCheckpoint.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

namespace gdl::game {
namespace {
constexpr f32 kMagnitude = 1'000'000;
constexpr s32 kMaxObject = 65535;

bool scalar(f32 value) {
    return std::isfinite(value) && std::abs(value) <= kMagnitude;
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
auto definition(const ItemInfo& value) {
    return std::tie(value.type, value.subtype, value.name, value.radius, value.height, value.xSize,
                    value.zSize, value.collisionType, value.collisionFlags, value.collisionOffset,
                    value.objectFlags, value.properties, value.value, value.armor, value.hitPoints,
                    value.activeType, value.activeOff, value.activeOn, value.choices);
}
bool sameInfos(std::span<const ItemInfo> a, std::span<const ItemInfo> b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](const auto& x, const auto& y) { return definition(x) == definition(y); });
}
bool samePlacement(const ItemInstance& a, const ItemInstance& b) {
    if (std::tie(a.info, a.minPlayers, a.flags, a.name, a.position, a.rotation, a.params) !=
            std::tie(b.info, b.minPlayers, b.flags, b.name, b.position, b.rotation, b.params) ||
        a.collision.size() != b.collision.size()) {
        return false;
    }
    for (usize index = 0; index < a.collision.size(); ++index) {
        if (a.collision[index].normal != b.collision[index].normal ||
            a.collision[index].vertices != b.collision[index].vertices) {
            return false;
        }
    }
    return true;
}
auto shape(const Obstacle& value) {
    return std::tie(value.centre, value.yaw, value.halfAcross, value.halfAlong, value.height,
                    value.solid, value.cylinderRadius);
}
bool sameShape(const Obstacle& a, const Obstacle& b) {
    return shape(a) == shape(b) && a.enemyItem.has_value() == b.enemyItem.has_value() &&
           (!a.enemyItem || (a.enemyItem->cylindrical == b.enemyItem->cylindrical &&
                             a.enemyItem->radius == b.enemyItem->radius));
}
bool shapeValid(const Obstacle& value) {
    return vector(value.centre) && scalar(value.yaw) && scalar(value.halfAcross) &&
           value.halfAcross >= 0 && scalar(value.halfAlong) && value.halfAlong >= 0 &&
           scalar(value.height) && value.height >= 0 && scalar(value.cylinderRadius) &&
           value.cylinderRadius >= 0 &&
           (!value.enemyItem || (scalar(value.enemyItem->radius) && value.enemyItem->radius >= 0));
}
auto barrelValues(const BreakablesCheckpoint::Barrel& value) {
    return std::tie(value.instance, value.kind, value.health, value.armor, value.contents,
                    value.count, value.minPlayers, value.state, value.breakingSeconds, value.shown,
                    value.gone, value.radius, value.height, value.collisionOffset, value.figure,
                    value.support);
}
std::optional<BreakableStrike::Kind> kindOf(const ItemInfo& info) {
    using Kind = BreakableStrike::Kind;
    if (info.type == ItemInfo::kContainer && info.subtype == Breakables::kBarrel) {
        return Kind::Holding;
    }
    if (info.type == Breakables::kBreakable) {
        switch (info.subtype) {
        case Breakables::kBarrel: return Kind::Plain;
        case Breakables::kExploding: return Kind::Exploding;
        case Breakables::kPoison: return Kind::Poison;
        default: break;
        }
    }
    return std::nullopt;
}
} // namespace

bool BreakablesCheckpoint::Support::valid() const {
    return object >= -1 && object <= kMaxObject && matrix(local) && vector(boxOffset) &&
           scalar(boxYaw);
}
bool BreakablesCheckpoint::Barrel::operator==(const Barrel& other) const {
    return barrelValues(*this) == barrelValues(other) &&
           samePlacement(placement, other.placement) && sameShape(box, other.box);
}
bool BreakablesCheckpoint::State::operator==(const State& other) const {
    return seed == other.seed && collision == other.collision && layout == other.layout &&
           barrels == other.barrels && sameInfos(infos, other.infos);
}
bool BreakablesCheckpoint::State::valid() const {
    if (barrels.size() > kMaxBarrels || infos.size() > kMaxInfos ||
        (!layout && (!barrels.empty() || !infos.empty()))) {
        return false;
    }
    usize choices = 0;
    for (const auto& info : infos) {
        if (info.name.size() > kMaxName || info.name.find('\0') != std::string::npos ||
            !scalar(info.radius) || !scalar(info.height) || !scalar(info.xSize) ||
            !scalar(info.zSize) || !vector(info.collisionOffset) ||
            info.choices.size() > kMaxChoices - choices) {
            return false;
        }
        choices += info.choices.size();
    }
    s32 previous = -1;
    usize triangles = 0;
    for (const auto& barrel : barrels) {
        const auto& placement = barrel.placement;
        if (barrel.instance <= previous || barrel.instance > kMaxObject || placement.info < 0 ||
            static_cast<usize>(placement.info) >= infos.size() ||
            placement.minPlayers != barrel.minPlayers || placement.name.size() > kMaxName ||
            placement.name.find('\0') != std::string::npos || !vector(placement.position) ||
            !vector(placement.rotation) ||
            placement.collision.size() > kMaxCollisionTriangles - triangles || barrel.health < 0 ||
            barrel.contents < std::numeric_limits<s16>::min() || barrel.contents > kMaxObject ||
            barrel.count < std::numeric_limits<s16>::min() ||
            barrel.count > std::numeric_limits<s16>::max() || barrel.minPlayers < 0 ||
            barrel.minPlayers > 255 || barrel.state < Breakables::kWhole ||
            barrel.state > Breakables::kBroken || !scalar(barrel.breakingSeconds) ||
            barrel.breakingSeconds < 0 || !scalar(barrel.radius) || barrel.radius <= 0 ||
            !scalar(barrel.height) || barrel.height <= 0 || !vector(barrel.collisionOffset) ||
            !barrel.figure.valid() || !shapeValid(barrel.box) || !barrel.support.valid() ||
            (!collision && barrel.support.object >= 0)) {
            return false;
        }
        const auto& info = infos[static_cast<usize>(placement.info)];
        if (kindOf(info) != barrel.kind || info.armor != barrel.armor ||
            barrel.radius != (info.radius > 0 ? info.radius : 1) ||
            barrel.height != (info.height > 0 ? info.height : 3) ||
            barrel.collisionOffset != info.collisionOffset) {
            return false;
        }
        triangles += placement.collision.size();
        for (const auto& triangle : placement.collision) {
            if (!vector(triangle.normal) || !std::ranges::all_of(triangle.vertices, vector)) {
                return false;
            }
        }
        previous = barrel.instance;
    }
    return true;
}

struct BreakablesCheckpoint::Prepared::Data {
    struct Resource {
        const ItemArchive* archive = nullptr;
        const TreeInfo* tree = nullptr;
        std::string name;
    };
    const BreakablesCheckpoint* wrapper = nullptr;
    const Breakables* owner = nullptr;
    const WorldLayout* layout = nullptr;
    const WorldCollision* collision = nullptr;
    std::shared_ptr<const u8> lifetime;
    std::shared_ptr<const u8> binding;
    u64 generation = 0;
    State state;
    std::vector<const Breakables::Barrel*> roster;
    std::vector<Resource> resources;
    std::vector<Breakables::Barrel> barrels;
};

BreakablesCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
BreakablesCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
BreakablesCheckpoint::Prepared&
BreakablesCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
BreakablesCheckpoint::Prepared::~Prepared() = default;
BreakablesCheckpoint::BreakablesCheckpoint(Breakables& owner) : m_owner(owner) {}
BreakablesCheckpoint::~BreakablesCheckpoint() = default;

std::optional<BreakablesCheckpoint::State> BreakablesCheckpoint::checkpoint() const {
    State result;
    result.infos = m_owner.m_infos;
    result.seed = m_owner.m_sim.seed;
    result.collision = m_owner.m_collision != nullptr;
    result.layout = m_owner.m_layout != nullptr;
    for (const auto& source : m_owner.m_sim.barrels) {
        if (!m_owner.m_layout || source.instance < 0 ||
            static_cast<usize>(source.instance) >= m_owner.m_layout->itemInstances().size()) {
            return std::nullopt;
        }
        auto figure = ItemFigureCheckpoint::capture(source.figure);
        if (!figure) {
            return std::nullopt;
        }
        const auto& support = source.support;
        result.barrels.push_back(
            {source.instance,
             m_owner.m_layout->itemInstances()[static_cast<usize>(source.instance)],
             source.kind,
             source.health,
             source.armor,
             source.contents,
             source.count,
             source.minPlayers,
             source.state,
             source.breakingSeconds,
             source.shown,
             source.gone,
             source.radius,
             source.height,
             source.collisionOffset,
             std::move(*figure),
             source.box,
             {support.m_object, support.m_local, support.m_boxOffset, support.m_boxYaw}});
    }
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}

std::optional<BreakablesCheckpoint::Prepared>
BreakablesCheckpoint::prepareRestore(const State& state) const {
    if (!state.valid() || state.barrels.size() != m_owner.m_sim.barrels.size() ||
        !sameInfos(state.infos, m_owner.m_infos) ||
        state.collision != (m_owner.m_collision != nullptr) ||
        state.layout != (m_owner.m_layout != nullptr) ||
        m_owner.m_checkpointGeneration == std::numeric_limits<u64>::max()) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->wrapper = this;
    data->owner = &m_owner;
    data->layout = m_owner.m_layout;
    data->collision = m_owner.m_collision;
    data->lifetime = m_lifetime;
    data->binding = m_owner.m_checkpointBinding;
    data->generation = m_owner.m_checkpointGeneration;
    data->state = state;
    for (usize index = 0; index < state.barrels.size(); ++index) {
        const auto& saved = state.barrels[index];
        const auto& source = m_owner.m_sim.barrels[index];
        if (source.instance != saved.instance || source.kind != saved.kind ||
            source.count != saved.count || source.minPlayers != saved.minPlayers ||
            !m_owner.m_layout ||
            static_cast<usize>(saved.instance) >= m_owner.m_layout->itemInstances().size() ||
            !samePlacement(saved.placement,
                           m_owner.m_layout->itemInstances()[static_cast<usize>(saved.instance)])) {
            return std::nullopt;
        }
        auto figure = ItemFigureCheckpoint::prepare(source.figure, saved.figure);
        if (!figure) {
            return std::nullopt;
        }
        const auto presentation = source.figure.presentation();
        data->resources.push_back({presentation.archive, presentation.tree,
                                   presentation.tree ? presentation.tree->name : ""});
        data->roster.push_back(&source);
        Breakables::Barrel barrel;
        barrel.instance = saved.instance;
        barrel.kind = saved.kind;
        barrel.health = saved.health;
        barrel.armor = saved.armor;
        barrel.contents = saved.contents;
        barrel.count = saved.count;
        barrel.minPlayers = saved.minPlayers;
        barrel.state = saved.state;
        barrel.breakingSeconds = saved.breakingSeconds;
        barrel.shown = saved.shown;
        barrel.gone = saved.gone;
        barrel.radius = saved.radius;
        barrel.height = saved.height;
        barrel.collisionOffset = saved.collisionOffset;
        barrel.figure = std::move(*figure);
        barrel.box = saved.box;
        barrel.support.m_object = saved.support.object;
        barrel.support.m_local = saved.support.local;
        barrel.support.m_boxOffset = saved.support.boxOffset;
        barrel.support.m_boxYaw = saved.support.boxYaw;
        data->barrels.push_back(std::move(barrel));
    }
    Prepared prepared{std::move(data)};
    return canCommit(prepared) ? std::optional{std::move(prepared)} : std::nullopt;
}

bool BreakablesCheckpoint::canCommit(const Prepared& prepared) const {
    if (!prepared.m_data) {
        return false;
    }
    const auto& data = *prepared.m_data;
    if (data.wrapper != this || data.owner != &m_owner || data.lifetime != m_lifetime ||
        data.binding != m_owner.m_checkpointBinding ||
        data.generation != m_owner.m_checkpointGeneration ||
        data.generation == std::numeric_limits<u64>::max() || data.layout != m_owner.m_layout ||
        data.collision != m_owner.m_collision ||
        data.roster.size() != m_owner.m_sim.barrels.size() ||
        !sameInfos(data.state.infos, m_owner.m_infos)) {
        return false;
    }
    for (usize index = 0; index < data.roster.size(); ++index) {
        if (data.roster[index] != &m_owner.m_sim.barrels[index]) {
            return false;
        }
        const auto instance = m_owner.m_sim.barrels[index].instance;
        if (!data.layout || instance < 0 ||
            static_cast<usize>(instance) >= data.layout->itemInstances().size() ||
            !samePlacement(data.state.barrels[index].placement,
                           data.layout->itemInstances()[static_cast<usize>(instance)])) {
            return false;
        }
    }
    return std::ranges::all_of(data.resources, [](const auto& resource) {
        if (resource.tree == nullptr) {
            return true;
        }
        const auto tree = resource.archive && resource.archive->loaded()
                              ? resource.archive->trees.find(resource.name)
                              : std::nullopt;
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): a found tree implies its archive
        return tree.has_value() && &resource.archive->trees.tree(*tree) == resource.tree;
    });
}
bool BreakablesCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    m_owner.m_sim.barrels.swap(data->barrels);
    m_owner.m_sim.seed = data->state.seed;
    ++m_owner.m_checkpointGeneration;
    return true;
}
bool BreakablesCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl::game
