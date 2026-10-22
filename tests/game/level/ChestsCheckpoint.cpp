#include "game/level/ChestsCheckpoint.h"

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

bool samePlacements(std::span<const ItemInstance> a, std::span<const ItemInstance> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), samePlacement);
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

auto chestValues(const ChestsCheckpoint::Chest& value) {
    return std::tie(value.instance, value.info, value.subtype, value.contents, value.count,
                    value.locked, value.state, value.opener, value.refusalLeft, value.minPlayers,
                    value.shown, value.held, value.gone, value.revealed, value.previewContents,
                    value.wobble, value.preview, value.figure, value.floor, value.collectedSeconds);
}
} // namespace

bool ChestsCheckpoint::Chest::operator==(const Chest& other) const {
    return chestValues(*this) == chestValues(other) && sameShape(box, other.box);
}

bool ChestsCheckpoint::State::operator==(const State& other) const {
    return seed == other.seed && collision == other.collision && layout == other.layout &&
           chests == other.chests && sameInfos(infos, other.infos) &&
           samePlacements(placements, other.placements);
}

bool ChestsCheckpoint::State::valid() const {
    if (infos.size() > kMaxInfos || chests.size() > kMaxChests ||
        placements.size() != chests.size() || (!layout && (!infos.empty() || !chests.empty()))) {
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
    for (usize index = 0; index < chests.size(); ++index) {
        const auto& chest = chests[index];
        const auto& placement = placements[index];
        if (placement.info != chest.info || placement.minPlayers != chest.minPlayers ||
            placement.name.size() > kMaxName || placement.name.find('\0') != std::string::npos ||
            !vector(placement.position) || !vector(placement.rotation) ||
            placement.collision.size() > kMaxCollisionTriangles - triangles) {
            return false;
        }
        triangles += placement.collision.size();
        for (const auto& triangle : placement.collision) {
            if (!vector(triangle.normal) || !std::ranges::all_of(triangle.vertices, vector)) {
                return false;
            }
        }
        const auto record = [&](s32 id) {
            return id >= -32768 && (id < 0 || static_cast<usize>(id) < infos.size());
        };
        if (chest.instance <= previous || chest.instance > static_cast<s32>(kMaxInfos) ||
            chest.info < 0 || static_cast<usize>(chest.info) >= infos.size() ||
            infos[static_cast<usize>(chest.info)].type != ItemInfo::kContainer ||
            !record(chest.contents) || !record(chest.previewContents) || chest.count < -32768 ||
            chest.count > 32767 || chest.state < Chests::kShut || chest.state > Chests::kOpen ||
            chest.opener < -1 || chest.opener >= 4 || !scalar(chest.refusalLeft) ||
            chest.refusalLeft < 0 || chest.refusalLeft > Chests::kRefusalSeconds ||
            chest.minPlayers < 0 || chest.minPlayers > 255 || chest.held < -1 ||
            chest.held > static_cast<s32>(kMagnitude) || !scalar(chest.wobble) ||
            !chest.preview.valid() || !chest.figure.valid() || !shapeValid(chest.box) ||
            (chest.floor &&
             (!collision || chest.floor->object < 0 ||
              chest.floor->object > static_cast<s32>(kMaxInfos) || !matrix(chest.floor->local))) ||
            (chest.collectedSeconds && (!scalar(*chest.collectedSeconds) ||
                                        *chest.collectedSeconds > Chests::kCollectedSeconds ||
                                        (*chest.collectedSeconds <= 0 && !chest.gone)))) {
            return false;
        }
        const s32 subtype = infos[static_cast<usize>(chest.info)].subtype;
        if (chest.subtype != subtype && chest.subtype != Chests::kTransmuted) {
            return false;
        }
        previous = chest.instance;
    }
    return true;
}

struct ChestsCheckpoint::Prepared::Data {
    struct Resource {
        const ItemArchive* archive = nullptr;
        const TreeInfo* tree = nullptr;
        std::string name;
    };
    const ChestsCheckpoint* wrapper = nullptr;
    const Chests* owner = nullptr;
    const WorldCollision* collision = nullptr;
    const WorldLayout* layout = nullptr;
    std::shared_ptr<const u8> lifetime;
    std::shared_ptr<const u8> binding;
    u64 generation = 0;
    u32 seed = 0;
    std::vector<ItemInfo> infos;
    std::vector<ItemInstance> placements;
    std::vector<const Chests::Chest*> roster;
    std::vector<Resource> resources;
    std::vector<Chests::Chest> chests;
};

ChestsCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
ChestsCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
ChestsCheckpoint::Prepared& ChestsCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
ChestsCheckpoint::Prepared::~Prepared() = default;
ChestsCheckpoint::ChestsCheckpoint(Chests& owner) : m_owner(owner) {}
ChestsCheckpoint::~ChestsCheckpoint() = default;

std::optional<ChestsCheckpoint::State> ChestsCheckpoint::checkpoint() const {
    State result;
    result.infos = m_owner.m_infos;
    result.seed = m_owner.m_sim.seed;
    result.collision = m_owner.m_collision != nullptr;
    result.layout = m_owner.m_layout != nullptr;
    for (const auto& source : m_owner.m_sim.chests) {
        if (!m_owner.m_layout || source.instance < 0 ||
            static_cast<usize>(source.instance) >= m_owner.m_layout->itemInstances().size()) {
            return std::nullopt;
        }
        result.placements.push_back(
            m_owner.m_layout->itemInstances()[static_cast<usize>(source.instance)]);
        auto figure = ItemFigureCheckpoint::capture(source.figure);
        auto preview = ItemFigureCheckpoint::capture(source.preview);
        if (!figure || !preview) {
            return std::nullopt;
        }
        result.chests.push_back(
            {source.instance,
             source.info,
             source.subtype,
             source.contents,
             source.count,
             source.locked,
             source.state,
             source.opener,
             source.refusalLeft,
             source.minPlayers,
             source.shown,
             source.held,
             source.gone,
             source.revealed,
             source.previewContents,
             source.wobble,
             std::move(*preview),
             std::move(*figure),
             source.box,
             source.floor ? std::optional{Floor{source.floor->object, source.floor->local}}
                          : std::nullopt,
             source.collectedSeconds});
    }
    if (!result.valid()) {
        return std::nullopt;
    }
    return result;
}

std::optional<ChestsCheckpoint::Prepared>
ChestsCheckpoint::prepareRestore(const State& state) const {
    if (!state.valid() || state.chests.size() != m_owner.m_sim.chests.size() ||
        !sameInfos(state.infos, m_owner.m_infos) ||
        state.collision != (m_owner.m_collision != nullptr) ||
        state.layout != (m_owner.m_layout != nullptr) ||
        m_owner.m_checkpointGeneration == std::numeric_limits<u64>::max()) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->wrapper = this;
    data->owner = &m_owner;
    data->collision = m_owner.m_collision;
    data->layout = m_owner.m_layout;
    data->lifetime = m_lifetime;
    data->binding = m_owner.m_checkpointBinding;
    data->generation = m_owner.m_checkpointGeneration;
    data->seed = state.seed;
    data->infos = state.infos;
    data->placements = state.placements;
    for (usize index = 0; index < state.chests.size(); ++index) {
        const auto& saved = state.chests[index];
        const auto& source = m_owner.m_sim.chests[index];
        if (source.instance != saved.instance || source.info != saved.info ||
            source.minPlayers != saved.minPlayers || source.count != saved.count ||
            source.locked != saved.locked || !m_owner.m_layout ||
            static_cast<usize>(saved.instance) >= m_owner.m_layout->itemInstances().size() ||
            !samePlacement(state.placements[index],
                           m_owner.m_layout->itemInstances()[static_cast<usize>(saved.instance)])) {
            return std::nullopt;
        }
        auto figure = ItemFigureCheckpoint::prepare(source.figure, saved.figure);
        auto preview = ItemFigureCheckpoint::prepare(source.preview, saved.preview);
        if (!figure || !preview) {
            return std::nullopt;
        }
        for (const auto* art : {&source.figure, &source.preview}) {
            const auto presentation = art->presentation();
            data->resources.push_back({presentation.archive, presentation.tree,
                                       presentation.tree ? presentation.tree->name : ""});
        }
        data->roster.push_back(&source);
        Chests::Chest chest;
        chest.instance = saved.instance;
        chest.info = saved.info;
        chest.subtype = saved.subtype;
        chest.contents = saved.contents;
        chest.count = saved.count;
        chest.locked = saved.locked;
        chest.state = saved.state;
        chest.opener = saved.opener;
        chest.refusalLeft = saved.refusalLeft;
        chest.minPlayers = saved.minPlayers;
        chest.shown = saved.shown;
        chest.held = saved.held;
        chest.gone = saved.gone;
        chest.revealed = saved.revealed;
        chest.previewContents = saved.previewContents;
        chest.wobble = saved.wobble;
        chest.preview = std::move(*preview);
        chest.figure = std::move(*figure);
        chest.box = saved.box;
        if (saved.floor) {
            chest.floor = Chests::Chest::Floor{saved.floor->object, saved.floor->local};
        }
        chest.collectedSeconds = saved.collectedSeconds;
        data->chests.push_back(std::move(chest));
    }
    Prepared prepared{std::move(data)};
    return canCommit(prepared) ? std::optional{std::move(prepared)} : std::nullopt;
}

bool ChestsCheckpoint::canCommit(const Prepared& prepared) const {
    if (!prepared.m_data) {
        return false;
    }
    const auto& data = *prepared.m_data;
    if (data.wrapper != this || data.owner != &m_owner || data.lifetime != m_lifetime ||
        data.binding != m_owner.m_checkpointBinding ||
        data.generation != m_owner.m_checkpointGeneration ||
        data.generation == std::numeric_limits<u64>::max() ||
        data.collision != m_owner.m_collision || data.layout != m_owner.m_layout ||
        data.roster.size() != m_owner.m_sim.chests.size() ||
        !sameInfos(data.infos, m_owner.m_infos)) {
        return false;
    }
    for (usize index = 0; index < data.roster.size(); ++index) {
        if (data.roster[index] != &m_owner.m_sim.chests[index]) {
            return false;
        }
        const auto instance = m_owner.m_sim.chests[index].instance;
        if (!data.layout || instance < 0 ||
            static_cast<usize>(instance) >= data.layout->itemInstances().size() ||
            !samePlacement(data.placements[index],
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

bool ChestsCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    m_owner.m_sim.chests.swap(data->chests);
    m_owner.m_sim.seed = data->seed;
    ++m_owner.m_checkpointGeneration;
    return true;
}

bool ChestsCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl::game
