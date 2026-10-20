#include "game/world/LockedGatesCheckpoint.h"

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
auto gateValues(const LockedGatesCheckpoint::Gate& value) {
    return std::tie(value.instance, value.subtype, value.state, value.openingTicks,
                    value.refusalLeft, value.minPlayers, value.shown, value.blocksPassage,
                    value.figure, value.support);
}
bool roster(const WorldLayout& layout, const LockedGatesCheckpoint::Gate& gate) {
    if (gate.instance < 0 || static_cast<usize>(gate.instance) >= layout.itemInstances().size()) {
        return false;
    }
    const auto& placement = layout.itemInstances()[static_cast<usize>(gate.instance)];
    return samePlacement(placement, gate.placement) && placement.info >= 0 &&
           static_cast<usize>(placement.info) < layout.itemInfos().size() &&
           infoValues(layout.itemInfos()[static_cast<usize>(placement.info)]) ==
               infoValues(gate.definition);
}
} // namespace

bool LockedGatesCheckpoint::Support::valid() const {
    return object >= -1 && object <= 65535 && matrix(local) && vector(boxOffset) && scalar(boxYaw);
}
bool LockedGatesCheckpoint::Gate::operator==(const Gate& other) const {
    return gateValues(*this) == gateValues(other) &&
           infoValues(definition) == infoValues(other.definition) &&
           samePlacement(placement, other.placement) && sameShape(box, other.box);
}
bool LockedGatesCheckpoint::State::valid() const {
    if (gates.size() > kMaxGates || (!layout && !gates.empty())) {
        return false;
    }
    s32 previous = -1;
    usize triangles = 0;
    for (const auto& gate : gates) {
        const auto& info = gate.definition;
        const auto& placement = gate.placement;
        if (gate.instance <= previous || gate.instance > 65535 || info.type != ItemInfo::kGate ||
            info.subtype != gate.subtype || info.name.size() > 64 ||
            info.name.find('\0') != std::string::npos || !scalar(info.radius) ||
            !scalar(info.height) || !scalar(info.xSize) || !scalar(info.zSize) ||
            !vector(info.collisionOffset) || !info.choices.empty() || placement.info < 0 ||
            placement.info > 65535 || placement.minPlayers != gate.minPlayers ||
            placement.name.size() > 64 || placement.name.find('\0') != std::string::npos ||
            !vector(placement.position) || !vector(placement.rotation) ||
            placement.collision.size() > kMaxCollisionTriangles - triangles ||
            gate.state < LockedGates::kShut || gate.state > LockedGates::kOpen ||
            gate.openingTicks < 0 || gate.openingTicks > static_cast<s32>(kMagnitude) ||
            !scalar(gate.refusalLeft) || gate.refusalLeft < 0 ||
            gate.refusalLeft > LockedGates::kRefusalSeconds || gate.minPlayers < 0 ||
            gate.minPlayers > 255 || !gate.figure.valid() || !shapeValid(gate.box) ||
            !gate.support.valid() || (!collision && gate.support.object >= 0)) {
            return false;
        }
        triangles += placement.collision.size();
        for (const auto& triangle : placement.collision) {
            if (!vector(triangle.normal) || !std::ranges::all_of(triangle.vertices, vector)) {
                return false;
            }
        }
        previous = gate.instance;
    }
    return true;
}

struct LockedGatesCheckpoint::Prepared::Data {
    struct Resource {
        const ItemArchive* archive = nullptr;
        const TreeInfo* tree = nullptr;
        std::string name;
    };
    const LockedGatesCheckpoint* wrapper = nullptr;
    const LockedGates* owner = nullptr;
    const WorldLayout* layout = nullptr;
    const WorldCollision* collision = nullptr;
    std::shared_ptr<const u8> lifetime;
    std::shared_ptr<const u8> binding;
    u64 generation = 0;
    State state;
    std::vector<const LockedGates::Gate*> roster;
    std::vector<Resource> resources;
    std::vector<LockedGates::Gate> gates;
};

LockedGatesCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
LockedGatesCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
LockedGatesCheckpoint::Prepared&
LockedGatesCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
LockedGatesCheckpoint::Prepared::~Prepared() = default;
LockedGatesCheckpoint::LockedGatesCheckpoint(LockedGates& owner) : m_owner(owner) {}
LockedGatesCheckpoint::~LockedGatesCheckpoint() = default;

std::optional<LockedGatesCheckpoint::State> LockedGatesCheckpoint::checkpoint() const {
    State result;
    result.collision = m_owner.m_collision != nullptr;
    result.layout = m_owner.m_layout != nullptr;
    for (const auto& source : m_owner.m_sim.gates) {
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
        result.gates.push_back(
            {source.instance,
             m_owner.m_layout->itemInfos()[static_cast<usize>(placement.info)],
             placement,
             source.subtype,
             source.state,
             source.openingTicks,
             source.refusalLeft,
             source.minPlayers,
             source.shown,
             source.blocksPassage,
             std::move(*figure),
             source.box,
             {support.m_object, support.m_local, support.m_boxOffset, support.m_boxYaw}});
    }
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}

std::optional<LockedGatesCheckpoint::Prepared>
LockedGatesCheckpoint::prepareRestore(const State& state) const {
    if (!state.valid() || state.gates.size() != m_owner.m_sim.gates.size() ||
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
    for (usize index = 0; index < state.gates.size(); ++index) {
        const auto& saved = state.gates[index];
        const auto& source = m_owner.m_sim.gates[index];
        if (!m_owner.m_layout || !roster(*m_owner.m_layout, saved) ||
            source.instance != saved.instance || source.subtype != saved.subtype ||
            source.minPlayers != saved.minPlayers || source.blocksPassage != saved.blocksPassage) {
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
        LockedGates::Gate gate;
        gate.instance = saved.instance;
        gate.subtype = saved.subtype;
        gate.state = saved.state;
        gate.openingTicks = saved.openingTicks;
        gate.refusalLeft = saved.refusalLeft;
        gate.minPlayers = saved.minPlayers;
        gate.shown = saved.shown;
        gate.blocksPassage = saved.blocksPassage;
        gate.figure = std::move(*figure);
        gate.box = saved.box;
        gate.support.m_object = saved.support.object;
        gate.support.m_local = saved.support.local;
        gate.support.m_boxOffset = saved.support.boxOffset;
        gate.support.m_boxYaw = saved.support.boxYaw;
        data->gates.push_back(std::move(gate));
    }
    Prepared prepared{std::move(data)};
    return canCommit(prepared) ? std::optional{std::move(prepared)} : std::nullopt;
}

bool LockedGatesCheckpoint::canCommit(const Prepared& prepared) const {
    if (!prepared.m_data) {
        return false;
    }
    const auto& data = *prepared.m_data;
    if (data.wrapper != this || data.owner != &m_owner || data.lifetime != m_lifetime ||
        data.binding != m_owner.m_checkpointBinding ||
        data.generation != m_owner.m_checkpointGeneration ||
        data.generation == std::numeric_limits<u64>::max() || data.layout != m_owner.m_layout ||
        data.collision != m_owner.m_collision || data.roster.size() != m_owner.m_sim.gates.size()) {
        return false;
    }
    for (usize index = 0; index < data.roster.size(); ++index) {
        if (data.roster[index] != &m_owner.m_sim.gates[index] || !data.layout ||
            !roster(*data.layout, data.state.gates[index])) {
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

bool LockedGatesCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    m_owner.m_sim.gates.swap(data->gates);
    ++m_owner.m_checkpointGeneration;
    return true;
}
bool LockedGatesCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl::game
