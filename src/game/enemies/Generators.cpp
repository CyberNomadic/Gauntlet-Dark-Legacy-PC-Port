#include "game/enemies/Generators.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <tuple>
#include <utility>

#include "engine/assets/AssetScope.h"
#include "engine/core/Types.h"

#include "game/world/ItemSupport.h"
#include "game/world/TargetAssist.h"

namespace gdl::game {

namespace {
constexpr usize kGeneratorKinds = 34;
/** lbl_8011BB20: a hit, by the kind bred. */
constexpr std::array<s32, kGeneratorKinds> kHitWorth{1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3,
                                                     1, 2, 2, 1, 2, 3, 1, 2, 3, 1, 1, 2,
                                                     3, 3, 3, 1, 0, 2, 1, 1, 2, 2};
/** lbl_8011BBA8: the destroying blow. */
constexpr std::array<s32, kGeneratorKinds> kDestroyedWorth{1, 2, 3, 1,  2, 3,  1, 2, 3,   1, 2, 3,
                                                           1, 2, 2, 1,  2, 3,  1, 2, 3,   1, 1, 2,
                                                           3, 3, 3, 15, 0, 30, 1, 2, 300, 30};
constexpr s32 kExperienceShare = 5;
} // namespace

s32 generatorExperience(s32 kind, bool destroyed) {
    constexpr s32 kSecondUnknown = -2;
    constexpr s32 kThirdUnknown = -3;
    if (kind == kSecondUnknown) {
        kind = 1;
    } else if (kind == kThirdUnknown) {
        kind = 2;
    } else if (kind < 0) {
        kind = 0;
    }
    if (static_cast<usize>(kind) >= kGeneratorKinds) {
        return 0;
    }
    const auto& worth = destroyed ? kDestroyedWorth : kHitWorth;
    return kExperienceShare * worth[static_cast<usize>(kind)];
}

namespace {

constexpr std::array<s32, 4> kTempleKinds{16, 23, 14, 13};
constexpr std::array<s32, 4> kHellKinds{2, 24, 20, 25};
constexpr std::array<s32, 4> kHellAlgorithms{30, 30, 7, 7};
// A generator's kind resolves as though bred at no strength: never the medium's second row.
constexpr s32 kGeneratorKindStrength = 0;
constexpr std::array<s32, 3> kCasterAlgorithms{28, 29, 30}; ///< dropped once a state crumbles
constexpr f32 kGeneratorViewScale = 8.0f;
constexpr f32 kAttentionRangeSquared = 2500.0f; ///< do_items, lbl_80343C50 (50 world units)

f32 flatDistance(const Vec3& a, const Vec3& b) {
    const f32 dx = a.x - b.x;
    const f32 dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

bool sameObstacle(const Obstacle& a, const Obstacle& b) {
    return a.centre == b.centre && a.yaw == b.yaw && a.halfAcross == b.halfAcross &&
           a.halfAlong == b.halfAlong && a.height == b.height && a.solid == b.solid &&
           a.cylinderRadius == b.cylinderRadius &&
           a.enemyItem.has_value() == b.enemyItem.has_value() &&
           (!a.enemyItem || (a.enemyItem->cylindrical == b.enemyItem->cylindrical &&
                             a.enemyItem->radius == b.enemyItem->radius));
}

bool sameView(const std::optional<ViewVolume>& a, const std::optional<ViewVolume>& b) {
    return a.has_value() == b.has_value() &&
           (!a || (a->position == b->position && a->right == b->right && a->up == b->up &&
                   a->forward == b->forward && a->sinX == b->sinX && a->cosX == b->cosX &&
                   a->sinY == b->sinY && a->cosY == b->cosY));
}

constexpr f32 kCheckpointMagnitude = 1'000'000;
bool bounded(f32 value, f32 minimum = -kCheckpointMagnitude, f32 maximum = kCheckpointMagnitude) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool bounded(const Vec3& value) {
    return bounded(value.x) && bounded(value.y) && bounded(value.z);
}

bool bounded(const Mat4& value) {
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            if (!bounded(value[column][row])) {
                return false;
            }
        }
    }
    return true;
}

bool generatorKind(s32 kind) {
    return kind == -3 || kind == -2 || (kind >= 0 && kind < kEnemyKindCount);
}

bool validSlot(const Generators::Slot& slot) {
    const auto& box = slot.box;
    return !slot.boss && slot.minPlayers >= -32768 && slot.minPlayers <= 32767 &&
           slot.presence <= Generators::Slot::Presence::Suppressed && generatorKind(slot.kind) &&
           slot.tier >= 0 && slot.tier <= 32767 && slot.algorithm >= -1 &&
           slot.algorithm <= 32767 && slot.most >= -1'000'000 && slot.most <= 1'000'000 &&
           slot.interval >= -1'000'000 && slot.interval <= 1'000'000 && bounded(slot.health, 0) &&
           bounded(slot.threshold, 0) && bounded(slot.armor) && slot.state >= 0 &&
           slot.state <= Generators::kStates && slot.countdown >= -100'000'000 &&
           slot.countdown <= 100'000'000 && bounded(slot.ratio, 0, 1) && slot.bred >= 0 &&
           slot.bred <= 100'000'000 && slot.living >= 0 && slot.living <= 100'000'000 &&
           bounded(slot.position) && bounded(slot.collisionOffset) &&
           bounded(slot.targetRadius, 0) && bounded(slot.targetHeight, 0) &&
           bounded(slot.placement) && bounded(slot.yaw) && bounded(slot.direction) &&
           bounded(slot.clearance, 0) && bounded(slot.patrolClearance, 0) &&
           bounded(slot.viewRadius, 0) && bounded(box.centre) && bounded(box.yaw) &&
           bounded(box.halfAcross, 0) && bounded(box.halfAlong, 0) && bounded(box.height, 0) &&
           bounded(box.cylinderRadius, 0) &&
           (!box.enemyItem || bounded(box.enemyItem->radius, 0)) && slot.support >= -1 &&
           bounded(slot.supportLocal) && bounded(slot.boxOffset) && bounded(slot.boxYaw);
}

// Only the native mutable fields may depart from the initially bound item.
// Support can be lost when its world object disappears, but never reassigned.
bool sameDefinition(const Generators::Slot& a, const Generators::Slot& b) {
    return a.kind == b.kind && a.minPlayers == b.minPlayers && a.interval == b.interval &&
           a.threshold == b.threshold && a.armor == b.armor &&
           a.collisionOffset == b.collisionOffset && a.targetRadius == b.targetRadius &&
           a.targetHeight == b.targetHeight && a.clearance == b.clearance &&
           a.patrolClearance == b.patrolClearance && a.viewRadius == b.viewRadius &&
           a.alwaysActive == b.alwaysActive && a.box.halfAcross == b.box.halfAcross &&
           a.box.halfAlong == b.box.halfAlong && a.box.height == b.box.height &&
           a.box.cylinderRadius == b.box.cylinderRadius &&
           a.box.enemyItem.has_value() == b.box.enemyItem.has_value() &&
           (!a.box.enemyItem || (a.box.enemyItem->cylindrical == b.box.enemyItem->cylindrical &&
                                 a.box.enemyItem->radius == b.box.enemyItem->radius)) &&
           (a.support == b.support || a.support == -1) && a.supportLocal == b.supportLocal &&
           a.boxOffset == b.boxOffset && a.boxYaw == b.boxYaw;
}

} // namespace

bool Generators::Slot::operator==(const Slot& other) const {
    const auto values = [](const Slot& s) {
        return std::tie(s.minPlayers, s.presence, s.kind, s.tier, s.algorithm, s.most, s.interval,
                        s.health, s.threshold, s.armor, s.state, s.countdown, s.ratio, s.bred,
                        s.living, s.position, s.collisionOffset, s.targetRadius, s.targetHeight,
                        s.placement, s.yaw, s.direction, s.clearance, s.patrolClearance,
                        s.viewRadius, s.alwaysActive, s.support, s.supportLocal, s.boxOffset,
                        s.boxYaw, s.boss);
    };
    return values(*this) == values(other) && sameObstacle(box, other.box);
}

bool Generators::Checkpoint::operator==(const Checkpoint& other) const {
    return roster == other.roster && generators == other.generators && bodies == other.bodies &&
           scales == other.scales && players == other.players &&
           specialBirth == other.specialBirth && sameView(view, other.view) &&
           attention == other.attention;
}

bool Generators::Checkpoint::valid() const {
    if (roster.size() > kMaxGenerators || generators.size() != roster.size() ||
        bodies.size() > static_cast<usize>(kEnemyKindCount) + 2 || players < 0 || players > 4 ||
        !bounded(scales.health, 0, 10000) || !bounded(scales.rate, 0, 10000) ||
        !bounded(scales.most, 0, 10000) || (attention && !bounded(*attention))) {
        return false;
    }
    if (view &&
        (!bounded(view->position) || !bounded(view->right) || !bounded(view->up) ||
         !bounded(view->forward) || !bounded(view->sinX, -1, 1) || !bounded(view->cosX, -1, 1) ||
         !bounded(view->sinY, -1, 1) || !bounded(view->cosY, -1, 1))) {
        return false;
    }
    for (usize index = 0; index < bodies.size(); ++index) {
        if (!generatorKind(bodies[index].kind) || !bodies[index].textures.valid()) {
            return false;
        }
        for (usize previous = 0; previous < index; ++previous) {
            if (bodies[index].kind == bodies[previous].kind) {
                return false;
            }
        }
    }
    for (usize index = 0; index < generators.size(); ++index) {
        const auto& slot = generators[index];
        if (!validSlot(slot) || !validSlot(roster[index]) || !sameDefinition(slot, roster[index]) ||
            std::ranges::none_of(bodies,
                                 [&](const auto& body) { return body.kind == slot.kind; })) {
            return false;
        }
    }
    return true;
}

std::optional<Generators::Checkpoint> Generators::checkpoint() const {
    if (!m_bound || std::ranges::any_of(m_generators, [](const auto& generator) {
            return generator.boss || generator.bossFigure != nullptr;
        })) {
        return std::nullopt;
    }
    Checkpoint result;
    result.roster = m_roster;
    result.generators.reserve(m_generators.size());
    for (const auto& generator : m_generators) {
        result.generators.push_back(generator);
    }
    result.bodies.reserve(m_bodies.size());
    for (const auto& body : m_bodies) {
        result.bodies.push_back({body->kind, body->textures.checkpoint()});
    }
    result.scales = m_scales;
    result.players = m_players;
    result.specialBirth = m_specialBirth;
    result.view = m_view;
    result.attention = m_attention;
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}

struct Generators::Prepared::Data {
    const Generators* owner = nullptr;
    std::shared_ptr<const u8> lifetime;
    u64 generation = 0;
    const WorldCollision* collision = nullptr;
    Checkpoint values;
    std::vector<Generator> generators;
    std::vector<TextureAnimator> textures;
};

Generators::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
Generators::Prepared::Prepared(Prepared&&) noexcept = default;
Generators::Prepared& Generators::Prepared::operator=(Prepared&&) noexcept = default;
Generators::Prepared::~Prepared() = default;

std::optional<Generators::Prepared> Generators::prepareRestore(const Checkpoint& state) const {
    if (!m_bound || !state.valid() || state.roster != m_roster ||
        state.generators.size() != m_generators.size() || state.bodies.size() != m_bodies.size() ||
        std::ranges::any_of(m_generators, [](const auto& generator) {
            return generator.boss || generator.bossFigure != nullptr;
        })) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->owner = this;
    data->lifetime = m_lifetime;
    data->generation = m_restoreGeneration;
    data->collision = m_collision;
    data->generators.resize(state.generators.size());
    for (usize index = 0; index < state.generators.size(); ++index) {
        const auto& slot = state.generators[index];
        if (slot.support >= 0 &&
            (m_collision == nullptr || !m_collision->objectTransform(slot.support))) {
            return std::nullopt;
        }
        static_cast<Slot&>(data->generators[index]) = slot;
    }
    data->textures.reserve(m_bodies.size());
    for (usize index = 0; index < m_bodies.size(); ++index) {
        if (state.bodies[index].kind != m_bodies[index]->kind) {
            return std::nullopt;
        }
        data->textures.push_back(m_bodies[index]->textures);
        if (!data->textures.back().restore(state.bodies[index].textures)) {
            return std::nullopt;
        }
    }
    data->values = state;
    return Prepared{std::move(data)};
}

bool Generators::canCommit(const Prepared& prepared) const {
    if (!prepared.m_data || prepared.m_data->owner != this ||
        prepared.m_data->lifetime != m_lifetime || !m_bound ||
        prepared.m_data->generation != m_restoreGeneration ||
        prepared.m_data->collision != m_collision ||
        prepared.m_data->generators.size() != m_generators.size() ||
        prepared.m_data->textures.size() != m_bodies.size()) {
        return false;
    }
    return std::ranges::none_of(prepared.m_data->generators, [&](const auto& slot) {
        return slot.support >= 0 &&
               (m_collision == nullptr || !m_collision->objectTransform(slot.support));
    });
}

bool Generators::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    m_generators = std::move(data->generators);
    for (usize index = 0; index < m_bodies.size(); ++index) {
        m_bodies[index]->textures = std::move(data->textures[index]);
    }
    m_scales = data->values.scales;
    m_players = data->values.players;
    m_specialBirth = data->values.specialBirth;
    m_view = data->values.view;
    m_attention = data->values.attention;
    ++m_restoreGeneration;
    return true;
}

bool Generators::restore(const Checkpoint& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}

s32 Generators::paramOf(const ItemInstance& instance, usize index) {
    if (index * 2 + 1 >= instance.params.size()) {
        return 0;
    }
    s16 value = 0;
    std::memcpy(&value, &instance.params[index * 2], sizeof(value));
    return value;
}

Generators::Bodies* Generators::bodiesOf(s32 kind) {
    for (auto& bodies : m_bodies) {
        if (bodies->kind == kind) {
            return bodies.get();
        }
    }
    return nullptr;
}

const Generators::Bodies* Generators::bodiesOf(s32 kind) const {
    for (const auto& bodies : m_bodies) {
        if (bodies->kind == kind) {
            return bodies.get();
        }
    }
    return nullptr;
}

bool Generators::loadBodies(RenderDevice& device, Enemies& enemies, s32 kind,
                            ItemArchive* realmItems, std::span<TextureSet* const> lenders) {
    if (bodiesOf(kind) != nullptr) {
        return true;
    }
    if (kind >= 0 && !enemies.loadKind(kind)) {
        return false;
    }
    ItemArchive* archive = kind < -1 ? realmItems : enemies.archive(kind);
    if (archive == nullptr) {
        return false;
    }
    auto bodies = std::make_unique<Bodies>();
    bodies->kind = kind;
    bodies->textures.bind(archive->trees.textureAnimations(), archive->textures, device, lenders);
    const EnemyKind& info = enemyKind(kind);
    // The state's object: "GEN_GRU3", with the authored suffixes the original retries.
    // Whole is three; nought is the ruin.
    for (s32 state = 0; state <= kStates; ++state) {
        const std::string base =
            std::format("GEN_{}{}", kind < -1 ? "SPECIAL" : info.prefix, state);
        // SPECIAL generators are multi-node authored trees in the realm archive,
        // not a model from one of the four enemy species they produce.
        if (const auto tree = archive->trees.find(base)) {
            bodies->trees[static_cast<usize>(state)] = archive->trees.tree(*tree);
            bodies->models[static_cast<usize>(state)].bind(bodies->trees[static_cast<usize>(state)],
                                                           archive->models, archive->textures,
                                                           device, lenders);
            bodies->models[static_cast<usize>(state)].setFrame(0, 0);
            continue;
        }
        if (const auto object = AssetScope(*archive).findObject(base)) {
            TreeInfo& tree = bodies->trees[static_cast<usize>(state)];
            tree.name = base;
            TreeNodeInfo node;
            node.name = base;
            node.object = object->name;
            tree.nodes.push_back(node);
            bodies->models[static_cast<usize>(state)].bind(tree, archive->models, archive->textures,
                                                           device, lenders);
        }
    }
    m_bodies.push_back(std::move(bodies));
    ++m_restoreGeneration;
    return true;
}

bool Generators::bind(RenderDevice& device, const WorldLayout& layout, Enemies& enemies,
                      const WorldCollision* collision, const GeneratorScales& scales, s32 players,
                      std::span<const LevelEnemy> roster, s32 realm, ItemArchive* realmItems,
                      std::span<TextureSet* const> lenders) {
    clear();
    m_collision = collision;
    const auto authored = itemSupportWorld(layout, collision);
    m_scales = scales;
    m_players = players;
    const std::vector<ItemInfo>& infos = layout.itemInfos();
    for (const ItemInstance& instance : layout.itemInstances()) {
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size()) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<usize>(instance.info)];
        if (info.type != ItemInfo::kGenerator) {
            continue;
        }
        const bool special = info.name == "SPECIAL" && (realm == 5 || realm == 6);
        const auto named =
            special ? std::optional<s32>{realm == 5 ? -2 : -3} : enemyKindOf(info.name);
        if (!named.has_value()) {
            continue;
        }
        // Small-class placements use the LOW body before the level remaps their species.
        const ItemInfo* body = &info;
        if (*named == kRatKind) {
            const auto low =
                std::ranges::find_if(layout.itemInfos(), [&](const ItemInfo& candidate) {
                    return candidate.type == info.type && candidate.name == "LOW" &&
                           (info.subtype <= 0 || candidate.subtype == info.subtype);
                });
            if (low != layout.itemInfos().end()) {
                body = &*low;
            }
        }
        const s32 specialTier = realm == 5 ? 2 : 3;
        const s32 authoredStrength = std::max(paramOf(instance, 0), 1);
        const s32 strength = special ? specialTier : authoredStrength;
        const s32 kind = levelKindOf(roster, *named, kGeneratorKindStrength);
        if (special) {
            for (const s32 species : realm == 5 ? kTempleKinds : kHellKinds) {
                enemies.loadKind(species);
            }
        }
        if (!loadBodies(device, enemies, kind, realmItems, lenders)) {
            continue;
        }
        Generator generator;
        generator.minPlayers = instance.minPlayers;
        generator.presence = shownToParty(instance.minPlayers, players)
                                 ? Generator::Presence::Shown
                                 : Generator::Presence::Hidden;
        generator.kind = kind;
        // The strength is also the state it stands in: a strength-one generator is one state
        // from gone and looks it.
        generator.tier = strength;
        generator.state = std::min(strength, kStates);
        generator.algorithm = paramOf(instance, 1);
        if (generator.algorithm < 0) {
            generator.algorithm = enemyKind(kind).algorithm;
        }
        const auto tierIndex = static_cast<usize>(std::clamp(authoredStrength, 1, kStates) - 1);
        s32 most = paramOf(instance, 2);
        s32 interval = paramOf(instance, 3);
        if (most == 0) {
            most = kDefaultMost[tierIndex];
        }
        if (interval == 0) {
            interval = kDefaultInterval[tierIndex];
        }
        // Scaled the way the original truncates them: to a whole count and interval.
        generator.most = static_cast<s32>(static_cast<f32>(most) * scales.most);
        generator.interval = static_cast<s32>(static_cast<f32>(interval) * scales.rate);
        generator.threshold = std::trunc(static_cast<f32>(body->hitPoints) * scales.health);
        // Durability and defaults use the authored strength before a special brood
        // replaces its troop tier. Damage and initial health are whole hit points.
        generator.health =
            std::trunc(static_cast<f32>(info.hitPoints * authoredStrength) * scales.health);
        generator.armor = static_cast<f32>(info.armor);
        const Mat4 placement = itemPlacement(instance.position, instance.rotation);
        generator.position = instance.position;
        if (collision != nullptr && (info.collisionFlags & 1U) == 0) {
            if (const auto floor = collision->floorAt(instance.position, 3.0f, 6.0f)) {
                // AddItemSub uses the same floor lift as other placed items.
                // Ground portals otherwise share the floor's depth exactly.
                generator.position.y = floor->y + ItemFigure::kFloorLift;
            }
        }
        generator.yaw = std::atan2(placement[2][0], placement[2][2]);
        generator.placement = placement;
        generator.placement[3] = Vec4{generator.position, 1};
        generator.direction = Vec3{placement[2][0], 0.0f, placement[2][2]};
        if (glm::length(generator.direction) > 0.001f) {
            generator.direction = glm::normalize(generator.direction);
        } else {
            generator.direction = Vec3{0.0f, 0.0f, 1.0f};
        }
        // SetItem copies the original offset before replacing a rat's info by LOW.
        generator.collisionOffset = info.collisionOffset;
        generator.targetRadius = body->radius;
        generator.targetHeight = body->height;
        generator.clearance = body->height;
        generator.patrolClearance = body->radius;
        generator.viewRadius = kGeneratorViewScale * std::max(info.radius, info.height);
        generator.alwaysActive = (info.activeType & 0x40U) != 0 || (instance.flags & 1U) != 0;
        generator.box.centre = generator.position;
        generator.box.yaw = generator.yaw;
        generator.box.halfAcross = body->xSize > 0.0f ? body->xSize : body->radius;
        generator.box.halfAlong = body->zSize > 0.0f ? body->zSize : body->radius;
        generator.box.height = body->height;
        generator.box.cylinderRadius = body->collisionType == 1 ? body->radius : 0.0f;
        if (body->collisionType == 1 || body->collisionType == 3) {
            generator.box.enemyItem = Obstacle::ItemQuery{body->collisionType == 1, body->radius};
        }
        generator.box.solid = generator.presence == Generator::Presence::Shown;
        bindSupport(generator, info, instance.position,
                    itemSupportAt(instance.position, collision, authored ? &*authored : nullptr));
        generator.countdown = 0;
        m_generators.push_back(std::move(generator));
    }
    syncFloors();
    m_roster.reserve(m_generators.size());
    for (const auto& generator : m_generators) {
        m_roster.push_back(generator);
    }
    m_bound = true;
    return true;
}

void Generators::bindSupport(Generator& generator, const ItemInfo& info, const Vec3& authored,
                             const WorldCollision* collision) {
    if (collision == nullptr || (info.collisionFlags & 1U) != 0) {
        return;
    }
    const auto floor = collision->floorAt(authored, 4, 10, 1);
    if (!floor) {
        return;
    }
    const auto transform = collision->objectTransform(floor->object);
    if (!transform) {
        return;
    }
    generator.boxOffset = Vec3{glm::inverse(generator.placement) * Vec4{generator.box.centre, 1}};
    generator.boxYaw = generator.box.yaw - generator.yaw;
    Mat4 placement = generator.placement;
    placement[3] = Vec4{authored.x, floor->y + ItemFigure::kFloorLift, authored.z, 1};
    generator.support = floor->object;
    generator.supportLocal = glm::inverse(*transform) * placement;
}

void Generators::syncFloors() {
    if (m_collision == nullptr) {
        return;
    }
    for (Generator& generator : m_generators) {
        if (generator.support < 0) {
            continue;
        }
        const auto transform = m_collision->objectTransform(generator.support);
        if (!transform) {
            generator.support = -1;
            continue;
        }
        // Disabling a floor's collision does not detach its scenery children.
        generator.placement = *transform * generator.supportLocal;
        generator.position = Vec3{generator.placement[3]};
        generator.yaw = std::atan2(generator.placement[2].x, generator.placement[2].z);
        generator.direction = Vec3{std::sin(generator.yaw), 0, std::cos(generator.yaw)};
        generator.box.centre = Vec3{generator.placement * Vec4{generator.boxOffset, 1}};
        generator.box.yaw = generator.yaw + generator.boxYaw;
        if (generator.bossFigure != nullptr) {
            generator.bossFigure->placeAt(generator.placement);
        }
    }
}

bool Generators::placeBoss(RenderDevice& device, const ItemInfo& info, ItemArchive& items,
                           Enemies& enemies, s32 kind, const Mat4& placement,
                           const WorldCollision* collision, std::string_view tree, bool settled) {
    if (info.type != ItemInfo::kGenerator || info.name != "BOSSGEN" || kind < 0 ||
        kind >= kEnemyKindCount || !enemies.loadKind(kind)) {
        return false;
    }
    Generator generator;
    generator.boss = true;
    generator.kind = kind;
    generator.tier = 1;
    generator.state = 1;
    // The default instance has strength one and AI zero. Its forty-tick item
    // animation startup is separate from the brood counter, which starts ready.
    generator.algorithm = 0;
    generator.countdown = 0;
    generator.viewRadius = kGeneratorViewScale * std::max(info.radius, info.height);
    generator.alwaysActive = (info.activeType & 0x40U) != 0;
    generator.most = static_cast<s32>(static_cast<f32>(kDefaultMost[0]) * m_scales.most);
    generator.interval = static_cast<s32>(static_cast<f32>(kDefaultInterval[0]) * m_scales.rate);
    generator.health = generator.threshold =
        std::trunc(static_cast<f32>(info.hitPoints) * m_scales.health);
    generator.armor = static_cast<f32>(info.armor);
    ItemInstance instance;
    instance.position = Vec3{placement[3]};
    const f32 yaw = std::atan2(placement[2].x, placement[2].z);
    instance.rotation.y = -yaw;
    generator.bossFigure = std::make_unique<ItemFigure>();
    // AtreeMatchAnyHeader also searches the summoned species' loaded archive.
    // The Genie's BOSSGEN tree lives with WIND, not with DJINN or the level items.
    AssetScope scope(items);
    if (ItemArchive* brood = enemies.archive(kind); brood != nullptr) {
        scope.add(*brood);
    }
    ItemArchive* art = scope.archiveForTree(tree);
    // A missing BOSSGEN tree does not prevent PlaceItem from creating its gameplay object.
    generator.bossFigure->place(device, art != nullptr ? *art : items, tree, instance, collision);
    generator.bossFigure->play(0, !settled);
    if (settled) {
        // The projectile has already finished its landing before it leaves the generator.
        generator.bossFigure->update(static_cast<f32>(generator.bossFigure->ticksOf(0)) / 60.0f);
    }
    generator.position = generator.bossFigure->position();
    generator.placement = placement;
    generator.placement[3] = Vec4{generator.position, 1};
    generator.yaw = yaw;
    generator.direction = Vec3{std::sin(generator.yaw), 0, std::cos(generator.yaw)};
    generator.collisionOffset = info.collisionOffset;
    generator.targetRadius = info.radius;
    generator.targetHeight = info.height;
    generator.clearance = info.height;
    generator.patrolClearance = info.radius;
    generator.box = generator.bossFigure->obstacle(info);
    bindSupport(generator, info, instance.position, collision);
    if (collision != nullptr) {
        m_collision = collision;
    }
    m_generators.push_back(std::move(generator));
    ++m_restoreGeneration;
    syncFloors();
    return true;
}

void Generators::clear() {
    ++m_restoreGeneration;
    m_bound = false;
    m_roster.clear();
    m_collision = nullptr;
    m_view.reset();
    m_attention.reset();
    m_specialBirth = 0;
    m_generators.clear();
    for (auto& bodies : m_bodies) {
        for (TreeModel& model : bodies->models) {
            model.clear();
        }
    }
    m_bodies.clear();
}

bool Generators::seen(const Generator& generator) const {
    if (!m_view) {
        return true;
    }
    // SetItem's collision and attention anchors share the lifted, rotated offset.
    const Vec3 point =
        TargetAssist::itemAcquisition(generator.placement, generator.collisionOffset,
                                      generator.targetRadius, generator.targetHeight, 1)
            .point;
    if (!m_view->sees(point, generator.viewRadius)) {
        return false;
    }
    // do_items applies this extra gate in normal/debug camera modes (1/2), not
    // the legacy camera used by bosses. An expanded sphere alone admits distant huts.
    return !m_attention ||
           glm::dot(point - *m_attention, point - *m_attention) <= kAttentionRangeSquared;
}

void Generators::updatePresence(Generator& generator, bool seen) const {
    // do_items' minoff gate changes an existing item's presence offscreen. A hidden
    // placement encountered on screen is suppressed for the rest of the level.
    if (generator.presence == Generator::Presence::Hidden) {
        if (seen) {
            generator.presence = Generator::Presence::Suppressed;
        } else if (shownToParty(generator.minPlayers, m_players)) {
            generator.presence = Generator::Presence::Shown;
        }
    } else if (generator.presence == Generator::Presence::Shown && !seen &&
               !shownToParty(generator.minPlayers, m_players)) {
        generator.presence = Generator::Presence::Hidden;
    }
    generator.box.solid = generator.presence == Generator::Presence::Shown && generator.state > 0;
}

void Generators::applyBroodEvents(Enemies& enemies) {
    for (const auto& event : enemies.takeGeneratorEvents()) {
        if (event.generator < 0 || static_cast<usize>(event.generator) >= m_generators.size()) {
            continue;
        }
        auto& generator = m_generators[static_cast<usize>(event.generator)];
        switch (event.kind) {
        case EnemyGeneratorEvent::Kind::Born: ++generator.living; break;
        case EnemyGeneratorEvent::Kind::Detached:
            generator.living = std::max(generator.living - 1, 0);
            break;
        case EnemyGeneratorEvent::Kind::PatrolHit: generator.algorithm = 0; break;
        case EnemyGeneratorEvent::Kind::PatrolDetached:
            generator.algorithm = 0;
            generator.living = 0;
            break;
        }
    }
}

void Generators::update(s32 ticks, Enemies& enemies, std::span<const EnemyView> players,
                        std::span<const Obstacle> obstacles, bool timeStopped) {
    syncFloors();
    if (ticks <= 0) {
        return;
    }
    // Texture cycles belong to the archive, not to each placed generator or its
    // current damage state. Keep them running even when its brood cannot spawn.
    for (auto& bodies : m_bodies) {
        bodies->textures.advance(static_cast<f32>(ticks) / 60.0f);
    }
    // Refresh the entire obstacle roster before any generator attempts a birth.
    for (Generator& generator : m_generators) {
        updatePresence(generator, seen(generator));
    }
    // uncouple_enemy releases quota before the death animation. A patrol's
    // departure resets the counter, which cannot be reconstructed by a census.
    applyBroodEvents(enemies);
    for (usize g = 0; g < m_generators.size(); ++g) {
        Generator& generator = m_generators[g];
        const bool visible = seen(generator);
        if (generator.presence != Generator::Presence::Shown) {
            continue;
        }
        if (generator.bossFigure != nullptr && generator.state > 0) {
            generator.bossFigure->update(static_cast<f32>(ticks) / 60.0f);
        }
        if (generator.state <= 0 || generator.tier <= 0) {
            continue;
        }
        const bool patrol = generator.algorithm == kPatrolWay;
        if (!patrol && !visible && !generator.alwaysActive) {
            continue;
        }
        // do_items checks the living brood quota before generate_now ticks its timer.
        // A full brood freezes the remaining wait; a death frees a slot, not a free birth.
        // Patrol generators instead keep one sentry, with no ordinary quota or wait.
        if (patrol ? generator.living > 0
                   : generator.most <= 0 || generator.living >= generator.most) {
            continue;
        }
        if (!patrol && generator.countdown > 0) {
            generator.countdown -= ticks;
            continue;
        }
        if (timeStopped) {
            continue;
        }
        // Ordinary broods require a nearby player; a lone patrol sentry starts offscreen
        // as well, without waiting for the party to approach its post.
        bool near = false;
        for (const EnemyView& view : players) {
            near = near || (!view.hidden &&
                            glm::distance(view.position, generator.position) <= kActiveDistance);
        }
        if (!patrol && !near) {
            continue;
        }
        EnemySpawn spawn;
        spawn.kind = generator.kind;
        spawn.tier = generator.tier;
        spawn.algorithm = generator.algorithm;
        if (generator.kind == -2 || generator.kind == -3) {
            const usize at = m_specialBirth++ % kTempleKinds.size();
            spawn.kind = generator.kind == -2 ? kTempleKinds[at] : kHellKinds[at];
            spawn.tier = generator.kind == -2 ? 2 : 3;
            spawn.algorithm = generator.kind == -2 ? 7 : kHellAlgorithms[at];
            spawn.allBirthDirections = true;
        }
        spawn.position = generator.position;
        spawn.direction = generator.direction;
        spawn.clearance = patrol ? generator.patrolClearance : generator.clearance;
        spawn.patrolBirth = patrol;
        spawn.priority = visible || patrol ? EnemySpawn::Priority::Offscreen
                                           : EnemySpawn::Priority::FreeSlotOnly;
        spawn.generator = static_cast<s32>(g);
        if (generator.algorithm == kZigZagWay) {
            spawn.zigZagSide = (generator.bred & 1) == 0 ? 1 : -1;
        }
        std::vector<Obstacle> birthObstacles{obstacles.begin(), obstacles.end()};
        for (usize other = 0; other < m_generators.size(); ++other) {
            if (other != g && standing(static_cast<s32>(other))) {
                birthObstacles.push_back(m_generators[other].box);
            }
        }
        const auto child = enemies.spawn(spawn, players, birthObstacles);
        // Recycled slots release their previous generator even when birth fails.
        applyBroodEvents(enemies);
        if (!child.has_value()) {
            continue;
        }
        ++generator.bred;
        if (patrol) {
            continue;
        }
        // The next takes longer, the countdown stretched by a share that grows a birth at a
        // time and wraps.
        generator.countdown = static_cast<s32>(
            kCountdownScale * static_cast<f32>(generator.interval) * (1.0f + generator.ratio));
        generator.ratio += 1.0f / (2.0f * static_cast<f32>(generator.most));
        if (generator.ratio > 1.0f) {
            generator.ratio = 0.0f;
        }
    }
}

s32 Generators::stateFor(const Generator& generator, bool destroyed) {
    if (destroyed || generator.health <= 0.0f) {
        return 0;
    }
    if (generator.health <= generator.threshold) {
        return 1;
    }
    if (generator.health <= generator.threshold * 2.0f) {
        return 2;
    }
    return 3;
}

std::optional<GeneratorEvent> Generators::strike(s32 id, f32 power, [[maybe_unused]] s32 byPlayer) {
    if (id < 0 || static_cast<usize>(id) >= m_generators.size()) {
        return std::nullopt;
    }
    Generator& generator = m_generators[static_cast<usize>(id)];
    if (generator.state <= 0 || generator.presence != Generator::Presence::Shown ||
        generator.armor < 0.0f) {
        return std::nullopt;
    }
    f32 amount = power - generator.armor;
    if (amount <= 0.0f) {
        amount = 1.0f;
    }
    generator.health = std::max(generator.health - std::round(amount), 0.0f);
    const s32 state = stateFor(generator, false);
    const bool changed = state != generator.state;
    generator.state = state;
    // A crumbling state is also the strength it breeds at from then on, and it breeds twice
    // as many; a caster brood goes back to seeking (fn_8005C1DC).
    if (state != generator.tier) {
        generator.tier = state;
        if (state > 0) {
            generator.most *= 2;
        }
        if (std::ranges::find(kCasterAlgorithms, generator.algorithm) != kCasterAlgorithms.end()) {
            generator.algorithm = 0;
        }
    }
    GeneratorEvent event;
    event.generator = id;
    event.kind = generator.kind;
    event.state = state;
    event.position = generator.position;
    event.placement = generator.placement;
    event.stateChanged = changed;
    event.destroyed = state == 0;
    if (event.destroyed) {
        generator.box.solid = false;
    }
    return event;
}

std::optional<s32> Generators::struckBy(const Vec3& from, const Vec3& to, f32 radius) const {
    std::optional<s32> best;
    f32 bestDistance = 0.0f;
    const Vec3 sweep = to - from;
    const f32 length = glm::length(sweep);
    for (usize g = 0; g < m_generators.size(); ++g) {
        const Generator& generator = m_generators[g];
        if (generator.state <= 0 || generator.presence != Generator::Presence::Shown) {
            continue;
        }
        const f32 reach = std::max(generator.box.halfAcross, generator.box.halfAlong);
        const Vec3 centre = generator.position + Vec3{0.0f, 0.5f * generator.box.height, 0.0f};
        const f32 t =
            length > 0.001f
                ? std::clamp(glm::dot(centre - from, sweep) / (length * length), 0.0f, 1.0f)
                : 0.0f;
        const Vec3 nearest = from + sweep * t;
        if (flatDistance(nearest, centre) <= radius + reach &&
            std::abs(nearest.y - centre.y) <= radius + 0.5f * generator.box.height + 1.0f) {
            const f32 distance = glm::length(nearest - from);
            if (!best.has_value() || distance < bestDistance) {
                best = static_cast<s32>(g);
                bestDistance = distance;
            }
        }
    }
    return best;
}

std::vector<s32> Generators::within(const Vec3& centre, f32 radius) const {
    std::vector<s32> out;
    for (usize g = 0; g < m_generators.size(); ++g) {
        const Generator& generator = m_generators[g];
        if (generator.state <= 0 || generator.presence != Generator::Presence::Shown) {
            continue;
        }
        const f32 reach = std::max(generator.box.halfAcross, generator.box.halfAlong);
        if (glm::length(generator.position + Vec3{0.0f, 0.5f * generator.box.height, 0.0f} -
                        centre) <= radius + reach) {
            out.push_back(static_cast<s32>(g));
        }
    }
    return out;
}

std::vector<Obstacle> Generators::obstacles() const {
    std::vector<Obstacle> out;
    for (const Generator& generator : m_generators) {
        if (generator.state > 0 && generator.presence == Generator::Presence::Shown) {
            out.push_back(generator.box);
        }
    }
    return out;
}

std::vector<Generators::Contact> Generators::identifiedObstacles() const {
    std::vector<Contact> out;
    for (usize index = 0; index < m_generators.size(); ++index) {
        const auto& generator = m_generators[index];
        if (generator.state > 0 && generator.presence == Generator::Presence::Shown) {
            out.push_back({static_cast<u32>(index), generator.box});
        }
    }
    return out;
}

std::vector<Obstacle> Generators::enemyObstacles() const {
    std::vector<Obstacle> out;
    for (const Generator& generator : m_generators) {
        if (generator.state > 0 && generator.presence == Generator::Presence::Shown &&
            generator.kind != kWormKind) {
            out.push_back(generator.box);
        }
    }
    return out;
}

void Generators::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                      f32 presentationAlpha) {
    for (const Generator& generator : m_generators) {
        if (generator.presence != Generator::Presence::Shown) {
            continue;
        }
        if (generator.boss) {
            if (generator.state > 0 && generator.bossFigure != nullptr) {
                generator.bossFigure->draw(device, clip, lighting);
            }
            continue;
        }
        Bodies* bodies = bodiesOf(generator.kind);
        if (bodies == nullptr) {
            continue;
        }
        const auto state = static_cast<usize>(std::clamp(generator.state, 0, kStates));
        TreeModel& model = bodies->models[state];
        if (!model.bound()) {
            continue;
        }
        bodies->textures.apply(model, bodies->trees[state], 0, 0.0f,
                               bodies->textures.presentationOffset(presentationAlpha));
        model.draw(device, clip, generator.placement, lighting);
    }
}

std::vector<Generators::BodyResource> Generators::bodyResources() const {
    std::vector<BodyResource> result;
    for (const auto& bodies : m_bodies) {
        for (s32 state = 0; state <= kStates; ++state) {
            const auto index = static_cast<usize>(state);
            if (bodies->models[index].bound()) {
                result.push_back({bodies->kind, state, &bodies->trees[index],
                                  &bodies->models[index], &bodies->textures});
            }
        }
    }
    std::ranges::sort(result, {},
                      [](const auto& entry) { return std::pair{entry.kind, entry.state}; });
    return result;
}

std::vector<Generators::Presentation> Generators::presentation() const {
    std::vector<Presentation> result;
    for (usize index = 0; index < m_generators.size(); ++index) {
        if (!bodyShown(static_cast<s32>(index))) {
            continue;
        }
        const auto& generator = m_generators[index];
        const auto* bodies = bodiesOf(generator.kind);
        result.push_back({index, generator.kind, generator.state, generator.placement,
                          bodies != nullptr ? static_cast<f32>(bodies->textures.frame()) : 0,
                          generator.bossFigure.get()});
    }
    return result;
}

bool Generators::standing(s32 id) const {
    return id >= 0 && static_cast<usize>(id) < m_generators.size() &&
           m_generators[static_cast<usize>(id)].state > 0 &&
           m_generators[static_cast<usize>(id)].presence == Generator::Presence::Shown;
}

bool Generators::bodyShown(s32 id) const {
    if (id < 0 || static_cast<usize>(id) >= m_generators.size()) {
        return false;
    }
    const Generator& generator = m_generators[static_cast<usize>(id)];
    if (generator.presence != Generator::Presence::Shown) {
        return false;
    }
    if (generator.boss) {
        return generator.state > 0 && generator.bossFigure != nullptr &&
               generator.bossFigure->hasFigure();
    }
    const Bodies* bodies = bodiesOf(generator.kind);
    return bodies != nullptr &&
           bodies->models[static_cast<usize>(std::clamp(generator.state, 0, kStates))].bound();
}

s32 Generators::stateOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].state;
}
f32 Generators::healthOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].health;
}
s32 Generators::kindOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].kind;
}
s32 Generators::tierOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].tier;
}
s32 Generators::algorithmOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].algorithm;
}
s32 Generators::mostOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].most;
}
s32 Generators::intervalOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].interval;
}
s32 Generators::countdownOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].countdown;
}
s32 Generators::bredOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].bred;
}
const Vec3& Generators::positionOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].position;
}
MissileTarget Generators::target(s32 index, s32 id) const {
    const auto& generator = m_generators.at(static_cast<usize>(index));
    MissileTarget result{id, generator.position,
                         std::max(generator.box.halfAcross, generator.box.halfAlong),
                         generator.box.height};
    result.acquisition =
        TargetAssist::itemAcquisition(generator.placement, generator.collisionOffset,
                                      generator.targetRadius, generator.targetHeight, 1);
    result.acquisition->enabled = generator.armor != -1;
    return result;
}

const Obstacle& Generators::boxOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].box;
}

} // namespace gdl::game
