#include "game/world/SafeRocksCheckpoint.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

namespace gdl::game {
namespace {
constexpr f32 kMagnitude = 1'000'000;
constexpr usize kMaxName = 64;
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
bool nameValid(const std::string& value) {
    return value.size() <= kMaxName && value.find('\0') == std::string::npos;
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
auto rockValues(const SafeRocksCheckpoint::Rock& value) {
    return std::tie(value.instance, value.health, value.baseHealth, value.armor, value.tier,
                    value.minPlayers, value.shown, value.dormant, value.activationDelay,
                    value.position, value.placement, value.models);
}
auto animationValues(const TextureAnimationInfo& value) {
    return std::tie(value.name, value.frameName, value.texture, value.source, value.frames,
                    value.start, value.rate, value.offset, value.flag);
}
bool sameAnimations(std::span<const TextureAnimationInfo> a,
                    std::span<const TextureAnimationInfo> b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
               return animationValues(x) == animationValues(y);
           });
}
std::vector<TextureAnimationInfo> animationsOf(const ItemArchive* items,
                                               std::span<const u32> slots) {
    std::vector<TextureAnimationInfo> result;
    if (items != nullptr) {
        for (const auto& animation : items->trees.textureAnimations()) {
            if (animation.freeRunning() && animation.texture >= 0 &&
                std::ranges::find(slots, static_cast<u32>(animation.texture)) != slots.end()) {
                result.push_back(animation);
            }
        }
    }
    return result;
}
bool matchesAnimations(std::span<const TextureAnimationInfo> saved, const ItemArchive* items,
                       std::span<const u32> slots) {
    usize index = 0;
    if (items != nullptr) {
        for (const auto& animation : items->trees.textureAnimations()) {
            if (!animation.freeRunning() || animation.texture < 0 ||
                std::ranges::find(slots, static_cast<u32>(animation.texture)) == slots.end()) {
                continue;
            }
            if (index == saved.size() ||
                animationValues(saved[index]) != animationValues(animation)) {
                return false;
            }
            ++index;
        }
    }
    return index == saved.size();
}
bool roster(const WorldLayout& layout, const SafeRocksCheckpoint::Rock& rock) {
    if (rock.instance < 0 || static_cast<usize>(rock.instance) >= layout.itemInstances().size()) {
        return false;
    }
    const auto& source = layout.itemInstances()[static_cast<usize>(rock.instance)];
    return samePlacement(source, rock.source) && source.info >= 0 &&
           static_cast<usize>(source.info) < layout.itemInfos().size() &&
           infoValues(layout.itemInfos()[static_cast<usize>(source.info)]) ==
               infoValues(rock.definition);
}
std::array<bool, 4> modelsOf(const SafeRocks::Rock& rock) {
    std::array<bool, 4> result{};
    for (usize index = 0; index < result.size(); ++index) {
        result[index] = rock.models[index].bound();
    }
    return result;
}
} // namespace

bool SafeRocksCheckpoint::Rock::operator==(const Rock& other) const {
    return rockValues(*this) == rockValues(other) &&
           infoValues(definition) == infoValues(other.definition) &&
           samePlacement(source, other.source) && sameShape(obstacle, other.obstacle);
}
bool SafeRocksCheckpoint::State::operator==(const State& other) const {
    return rocks == other.rocks && textureSlots == other.textureSlots &&
           sameAnimations(animations, other.animations) &&
           resolvedAnimations == other.resolvedAnimations && textures == other.textures &&
           layout == other.layout && items == other.items && animationItems == other.animationItems;
}
bool SafeRocksCheckpoint::State::valid() const {
    if (rocks.size() > kMaxRocks || textureSlots.size() > kMaxSlots ||
        animations.size() > kMaxAnimations || resolvedAnimations > animations.size() ||
        (!layout && !rocks.empty()) || (!items && !rocks.empty()) ||
        (!animationItems && (!animations.empty() || resolvedAnimations != 0)) ||
        !textures.valid()) {
        return false;
    }
    auto sortedSlots = textureSlots;
    std::ranges::sort(sortedSlots);
    if ((!sortedSlots.empty() && sortedSlots.back() > kMaxSlots) ||
        std::ranges::adjacent_find(sortedSlots) != sortedSlots.end()) {
        return false;
    }
    for (const auto& animation : animations) {
        if (!nameValid(animation.name) || !nameValid(animation.frameName) ||
            !animation.freeRunning() || animation.texture < 0 ||
            !std::ranges::binary_search(sortedSlots, static_cast<u32>(animation.texture))) {
            return false;
        }
    }
    s32 previous = -1;
    usize triangles = 0;
    usize choices = 0;
    for (const auto& rock : rocks) {
        const auto& info = rock.definition;
        const auto& source = rock.source;
        const u32 override =
            static_cast<u32>(source.params[0]) | (static_cast<u32>(source.params[1]) << 8);
        const s32 subtype =
            override > 0 && override < 0x8000 ? static_cast<s32>(override) : info.subtype;
        if (rock.instance <= previous || rock.instance > kMaxObject ||
            info.type != SafeRocks::kItemType || subtype != SafeRocks::kSubtype ||
            !nameValid(info.name) || info.choices.size() > kMaxChoices - choices ||
            !scalar(info.radius) || !scalar(info.height) || !scalar(info.xSize) ||
            !scalar(info.zSize) || !vector(info.collisionOffset) || source.info < 0 ||
            source.info > kMaxObject || source.minPlayers != rock.minPlayers ||
            !nameValid(source.name) || !vector(source.position) || !vector(source.rotation) ||
            source.collision.size() > kMaxCollisionTriangles - triangles || rock.baseHealth < 0 ||
            rock.baseHealth > std::numeric_limits<s32>::max() / SafeRocks::kWhole ||
            rock.baseHealth != std::max(info.hitPoints, 0) || rock.health < 0 ||
            rock.health > SafeRocks::kWhole * rock.baseHealth || rock.armor != info.armor ||
            rock.tier < 0 || rock.tier > SafeRocks::kWhole || rock.minPlayers < 0 ||
            rock.minPlayers > 255 || !scalar(rock.activationDelay) || rock.activationDelay < 0 ||
            !vector(rock.position) || !matrix(rock.placement) || !shapeValid(rock.obstacle)) {
            return false;
        }
        triangles += source.collision.size();
        choices += info.choices.size();
        for (const auto& triangle : source.collision) {
            if (!vector(triangle.normal) || !std::ranges::all_of(triangle.vertices, vector)) {
                return false;
            }
        }
        previous = rock.instance;
    }
    return true;
}

struct SafeRocksCheckpoint::Prepared::Data {
    const SafeRocksCheckpoint* wrapper = nullptr;
    const SafeRocks* owner = nullptr;
    const WorldLayout* layout = nullptr;
    const ItemArchive* items = nullptr;
    const ItemArchive* animationItems = nullptr;
    std::shared_ptr<const u8> lifetime;
    std::shared_ptr<const u8> binding;
    u64 generation = 0;
    State state;
    std::vector<const SafeRocks::Rock*> roster;
    std::vector<SafeRocks::Rock> rocks;
    TextureAnimator textures;
};
SafeRocksCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
SafeRocksCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
SafeRocksCheckpoint::Prepared&
SafeRocksCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
SafeRocksCheckpoint::Prepared::~Prepared() = default;
SafeRocksCheckpoint::SafeRocksCheckpoint(SafeRocks& owner) : m_owner(owner) {}
SafeRocksCheckpoint::~SafeRocksCheckpoint() = default;

std::optional<SafeRocksCheckpoint::State> SafeRocksCheckpoint::checkpoint() const {
    State result;
    result.textureSlots = m_owner.m_textureSlots;
    result.textures = m_owner.m_textures.checkpoint();
    result.resolvedAnimations = m_owner.m_textures.size();
    result.layout = m_owner.m_layout != nullptr;
    result.items = m_owner.m_items != nullptr;
    result.animationItems = m_owner.m_animationItems != nullptr;
    result.animations = animationsOf(m_owner.m_animationItems, m_owner.m_textureSlots);
    for (const auto& source : m_owner.m_rocks) {
        if (!source || !m_owner.m_layout || source->instance < 0 ||
            static_cast<usize>(source->instance) >= m_owner.m_layout->itemInstances().size()) {
            return std::nullopt;
        }
        const auto& placement =
            m_owner.m_layout->itemInstances()[static_cast<usize>(source->instance)];
        if (placement.info < 0 ||
            static_cast<usize>(placement.info) >= m_owner.m_layout->itemInfos().size()) {
            return std::nullopt;
        }
        result.rocks.push_back(
            {source->instance, m_owner.m_layout->itemInfos()[static_cast<usize>(placement.info)],
             placement, source->health, source->baseHealth, source->armor, source->tier,
             source->minPlayers, source->shown, source->dormant, source->activationDelay,
             source->position, source->obstacle, source->placement, modelsOf(*source)});
    }
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}

std::optional<SafeRocksCheckpoint::Prepared>
SafeRocksCheckpoint::prepareRestore(const State& state) const {
    if (!state.valid() || state.rocks.size() != m_owner.m_rocks.size() ||
        state.textureSlots != m_owner.m_textureSlots ||
        state.resolvedAnimations != m_owner.m_textures.size() ||
        state.layout != (m_owner.m_layout != nullptr) ||
        state.items != (m_owner.m_items != nullptr) ||
        state.animationItems != (m_owner.m_animationItems != nullptr) ||
        !matchesAnimations(state.animations, m_owner.m_animationItems, m_owner.m_textureSlots) ||
        m_owner.m_checkpointGeneration == std::numeric_limits<u64>::max()) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->wrapper = this;
    data->owner = &m_owner;
    data->layout = m_owner.m_layout;
    data->items = m_owner.m_items;
    data->animationItems = m_owner.m_animationItems;
    data->lifetime = m_lifetime;
    data->binding = m_owner.m_checkpointBinding;
    data->generation = m_owner.m_checkpointGeneration;
    data->state = state;
    data->textures = m_owner.m_textures;
    if (!data->textures.restore(state.textures)) {
        return std::nullopt;
    }
    data->rocks.reserve(state.rocks.size());
    for (usize index = 0; index < state.rocks.size(); ++index) {
        const auto& saved = state.rocks[index];
        const auto& source = m_owner.m_rocks[index];
        if (!source || (m_owner.m_layout == nullptr) || !roster(*m_owner.m_layout, saved) ||
            source->instance != saved.instance || source->baseHealth != saved.baseHealth ||
            source->armor != saved.armor || source->minPlayers != saved.minPlayers ||
            source->position != saved.position || source->placement != saved.placement ||
            !sameShape(source->obstacle, saved.obstacle) || modelsOf(*source) != saved.models) {
            return std::nullopt;
        }
        data->roster.push_back(source.get());
        auto& rock = data->rocks.emplace_back(*source);
        rock.health = saved.health;
        rock.tier = saved.tier;
        rock.shown = saved.shown;
        rock.dormant = saved.dormant;
        rock.activationDelay = saved.activationDelay;
        for (auto& model : rock.models) {
            data->textures.apply(model);
        }
    }
    Prepared result{std::move(data)};
    return canCommit(result) ? std::optional{std::move(result)} : std::nullopt;
}

bool SafeRocksCheckpoint::canCommit(const Prepared& prepared) const {
    if (!prepared.m_data) {
        return false;
    }
    const auto& data = *prepared.m_data;
    if (data.wrapper != this || data.owner != &m_owner || data.lifetime != m_lifetime ||
        data.binding != m_owner.m_checkpointBinding ||
        data.generation != m_owner.m_checkpointGeneration ||
        data.generation == std::numeric_limits<u64>::max() || data.layout != m_owner.m_layout ||
        data.items != m_owner.m_items || data.animationItems != m_owner.m_animationItems ||
        data.roster.size() != m_owner.m_rocks.size() ||
        data.state.textureSlots != m_owner.m_textureSlots ||
        data.state.resolvedAnimations != m_owner.m_textures.size() ||
        !matchesAnimations(data.state.animations, m_owner.m_animationItems,
                           m_owner.m_textureSlots)) {
        return false;
    }
    for (usize index = 0; index < data.roster.size(); ++index) {
        if (data.roster[index] != m_owner.m_rocks[index].get() || (data.layout == nullptr) ||
            !roster(*data.layout, data.state.rocks[index]) ||
            modelsOf(*m_owner.m_rocks[index]) != data.state.rocks[index].models) {
            return false;
        }
    }
    return true;
}
bool SafeRocksCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    for (usize index = 0; index < data->rocks.size(); ++index) {
        *m_owner.m_rocks[index] = std::move(data->rocks[index]);
    }
    m_owner.m_textures = std::move(data->textures);
    ++m_owner.m_checkpointGeneration;
    return true;
}
bool SafeRocksCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl::game
