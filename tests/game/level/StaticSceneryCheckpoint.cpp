#include "game/level/StaticSceneryCheckpoint.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

namespace gdl::game {
namespace {
constexpr f32 kMagnitude = 1'000'000;
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
bool nameValid(const std::string& name) {
    return name.size() <= 64 && name.find('\0') == std::string::npos;
}
auto infoValues(const ItemInfo& value) {
    return std::tie(value.type, value.subtype, value.name, value.radius, value.height, value.xSize,
                    value.zSize, value.collisionType, value.collisionFlags, value.collisionOffset,
                    value.objectFlags, value.properties, value.value, value.armor, value.hitPoints,
                    value.activeType, value.activeOff, value.activeOn, value.choices);
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
auto propValues(const StaticSceneryCheckpoint::Prop& value) {
    return std::tie(value.instance, value.minPlayers, value.shown, value.figure, value.support);
}
bool roster(const WorldLayout& layout, const StaticSceneryCheckpoint::Prop& prop) {
    if (prop.instance < 0 || static_cast<usize>(prop.instance) >= layout.itemInstances().size()) {
        return false;
    }
    const auto& placement = layout.itemInstances()[static_cast<usize>(prop.instance)];
    return samePlacement(placement, prop.placement) && placement.info >= 0 &&
           static_cast<usize>(placement.info) < layout.itemInfos().size() &&
           infoValues(layout.itemInfos()[static_cast<usize>(placement.info)]) ==
               infoValues(prop.definition);
}
} // namespace

bool StaticSceneryCheckpoint::Support::valid() const {
    return object >= -1 && object <= 65535 && matrix(local) && vector(boxOffset) && scalar(boxYaw);
}
bool StaticSceneryCheckpoint::Prop::operator==(const Prop& other) const {
    return propValues(*this) == propValues(other) &&
           infoValues(definition) == infoValues(other.definition) &&
           samePlacement(placement, other.placement) && sameShape(box, other.box);
}
bool StaticSceneryCheckpoint::State::valid() const {
    if (props.size() > kMaxProps || (!layout && !props.empty())) {
        return false;
    }
    s32 previous = -1;
    usize triangles = 0;
    usize choices = 0;
    for (const auto& prop : props) {
        const auto& info = prop.definition;
        const auto& placement = prop.placement;
        const u32 override =
            static_cast<u32>(placement.params[0]) | (static_cast<u32>(placement.params[1]) << 8);
        const s32 subtype =
            override > 0 && override < 0x8000 ? static_cast<s32>(override) : info.subtype;
        if (prop.instance <= previous || prop.instance > 65535 || info.type != 10 || subtype != 0 ||
            !nameValid(info.name) || !scalar(info.radius) || !scalar(info.height) ||
            !scalar(info.xSize) || !scalar(info.zSize) || !vector(info.collisionOffset) ||
            info.choices.size() > kMaxChoices - choices || placement.info < 0 ||
            placement.info > 65535 || placement.minPlayers != prop.minPlayers ||
            !nameValid(placement.name) || !vector(placement.position) ||
            !vector(placement.rotation) ||
            placement.collision.size() > kMaxCollisionTriangles - triangles ||
            prop.minPlayers < 0 || prop.minPlayers > 255 || !prop.figure.valid() ||
            !shapeValid(prop.box) || !prop.support.valid() ||
            (!collision && prop.support.object >= 0)) {
            return false;
        }
        triangles += placement.collision.size();
        choices += info.choices.size();
        for (const auto& triangle : placement.collision) {
            if (!vector(triangle.normal) || !std::ranges::all_of(triangle.vertices, vector)) {
                return false;
            }
        }
        previous = prop.instance;
    }
    return true;
}

struct StaticSceneryCheckpoint::Prepared::Data {
    struct Resource {
        const ItemArchive* archive = nullptr;
        const TreeInfo* tree = nullptr;
    };
    const StaticSceneryCheckpoint* wrapper = nullptr;
    const StaticScenery* owner = nullptr;
    const WorldLayout* layout = nullptr;
    const WorldCollision* collision = nullptr;
    std::shared_ptr<const u8> lifetime;
    std::shared_ptr<const u8> binding;
    u64 generation = 0;
    State state;
    std::vector<const StaticScenery::Prop*> roster;
    std::vector<Resource> resources;
    std::vector<StaticScenery::Prop> props;
};

StaticSceneryCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
StaticSceneryCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
StaticSceneryCheckpoint::Prepared&
StaticSceneryCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
StaticSceneryCheckpoint::Prepared::~Prepared() = default;
StaticSceneryCheckpoint::StaticSceneryCheckpoint(StaticScenery& owner) : m_owner(owner) {}
StaticSceneryCheckpoint::~StaticSceneryCheckpoint() = default;

std::optional<StaticSceneryCheckpoint::State> StaticSceneryCheckpoint::checkpoint() const {
    State result;
    result.collision = m_owner.m_collision != nullptr;
    result.layout = m_owner.m_layout != nullptr;
    for (const auto& source : m_owner.m_sim.props) {
        if (!m_owner.m_layout || source.instance < 0 ||
            static_cast<usize>(source.instance) >= m_owner.m_layout->itemInstances().size()) {
            return std::nullopt;
        }
        const auto& placement =
            m_owner.m_layout->itemInstances()[static_cast<usize>(source.instance)];
        if (placement.info < 0 ||
            static_cast<usize>(placement.info) >= m_owner.m_layout->itemInfos().size()) {
            return std::nullopt;
        }
        auto figure = ItemFigureCheckpoint::capture(source.figure);
        if (!figure) {
            return std::nullopt;
        }
        const auto& support = source.support;
        result.props.push_back(
            {source.instance,
             m_owner.m_layout->itemInfos()[static_cast<usize>(placement.info)],
             placement,
             source.minPlayers,
             source.shown,
             std::move(*figure),
             {support.m_object, support.m_local, support.m_boxOffset, support.m_boxYaw},
             source.box});
    }
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}

std::optional<StaticSceneryCheckpoint::Prepared>
StaticSceneryCheckpoint::prepareRestore(const State& state) const {
    if (!state.valid() || state.props.size() != m_owner.m_sim.props.size() ||
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
    for (usize index = 0; index < state.props.size(); ++index) {
        const auto& saved = state.props[index];
        const auto& source = m_owner.m_sim.props[index];
        if (!m_owner.m_layout || !roster(*m_owner.m_layout, saved) ||
            source.instance != saved.instance || source.minPlayers != saved.minPlayers) {
            return std::nullopt;
        }
        auto figure = ItemFigureCheckpoint::prepare(source.figure, saved.figure);
        if (!figure) {
            return std::nullopt;
        }
        const auto presentation = source.figure.presentation();
        data->resources.push_back({presentation.archive, presentation.tree});
        data->roster.push_back(&source);
        StaticScenery::Prop prop;
        prop.instance = saved.instance;
        prop.minPlayers = saved.minPlayers;
        prop.shown = saved.shown;
        prop.figure = std::move(*figure);
        prop.box = saved.box;
        prop.support.m_object = saved.support.object;
        prop.support.m_local = saved.support.local;
        prop.support.m_boxOffset = saved.support.boxOffset;
        prop.support.m_boxYaw = saved.support.boxYaw;
        data->props.push_back(std::move(prop));
    }
    Prepared result{std::move(data)};
    return canCommit(result) ? std::optional{std::move(result)} : std::nullopt;
}

bool StaticSceneryCheckpoint::canCommit(const Prepared& prepared) const {
    if (!prepared.m_data) {
        return false;
    }
    const auto& data = *prepared.m_data;
    if (data.wrapper != this || data.owner != &m_owner || data.lifetime != m_lifetime ||
        data.binding != m_owner.m_checkpointBinding ||
        data.generation != m_owner.m_checkpointGeneration ||
        data.generation == std::numeric_limits<u64>::max() || data.layout != m_owner.m_layout ||
        data.collision != m_owner.m_collision || data.roster.size() != m_owner.m_sim.props.size()) {
        return false;
    }
    for (usize index = 0; index < data.roster.size(); ++index) {
        if (data.roster[index] != &m_owner.m_sim.props[index] || !data.layout ||
            !roster(*data.layout, data.state.props[index])) {
            return false;
        }
        const auto current = m_owner.m_sim.props[index].figure.presentation();
        const auto& resource = data.resources[index];
        // A static fallback tree is owned by the figure, not the archive tree
        // table. Compare the exact still-bound tree rather than a name lookup.
        if (current.archive != resource.archive || current.tree != resource.tree ||
            (resource.tree && (!resource.archive || !resource.archive->loaded()))) {
            return false;
        }
    }
    return true;
}
bool StaticSceneryCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    m_owner.m_sim.props.swap(data->props);
    ++m_owner.m_checkpointGeneration;
    return true;
}
bool StaticSceneryCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl::game
