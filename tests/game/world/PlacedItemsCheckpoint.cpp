#include "game/world/PlacedItemsCheckpoint.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>
#include <type_traits>
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
bool name(const std::string& value) {
    return value.size() <= 64 && value.find('\0') == std::string::npos;
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
    for (usize i = 0; i < a.collision.size(); ++i) {
        if (a.collision[i].normal != b.collision[i].normal ||
            a.collision[i].vertices != b.collision[i].vertices) {
            return false;
        }
    }
    return true;
}
bool sameDefinitions(const std::vector<ItemInfo>& a, const std::vector<ItemInfo>& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](const ItemInfo& x, const ItemInfo& y) {
               return infoValues(x) == infoValues(y);
           });
}
bool samePlacements(const std::vector<ItemInstance>& a, const std::vector<ItemInstance>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), samePlacement);
}
bool sameBits(f32 a, f32 b) {
    return std::bit_cast<u32>(a) == std::bit_cast<u32>(b);
}
bool sameBits(const Vec3& a, const Vec3& b) {
    return sameBits(a.x, b.x) && sameBits(a.y, b.y) && sameBits(a.z, b.z);
}
bool sameCaptureIdentity(const PlacedItemsCheckpoint::State& a,
                         const PlacedItemsCheckpoint::State& b) {
    if (a.layout != b.layout || a.archives != b.archives || a.figures != b.figures ||
        a.bursts != b.bursts || a.definitions.size() != b.definitions.size() ||
        a.placements.size() != b.placements.size()) {
        return false;
    }
    // Unlike restore's semantic equality, storage reuse must preserve the exact
    // captured representation, including signed zero in authored float fields.
    for (usize i = 0; i < a.definitions.size(); ++i) {
        const auto& x = a.definitions[i];
        const auto& y = b.definitions[i];
        if (std::tie(x.type, x.subtype, x.name, x.collisionType, x.collisionFlags, x.objectFlags,
                     x.properties, x.value, x.armor, x.hitPoints, x.activeType, x.activeOff,
                     x.activeOn, x.choices) !=
                std::tie(y.type, y.subtype, y.name, y.collisionType, y.collisionFlags,
                         y.objectFlags, y.properties, y.value, y.armor, y.hitPoints, y.activeType,
                         y.activeOff, y.activeOn, y.choices) ||
            !sameBits(x.radius, y.radius) || !sameBits(x.height, y.height) ||
            !sameBits(x.xSize, y.xSize) || !sameBits(x.zSize, y.zSize) ||
            !sameBits(x.collisionOffset, y.collisionOffset)) {
            return false;
        }
    }
    for (usize i = 0; i < a.placements.size(); ++i) {
        const auto& x = a.placements[i];
        const auto& y = b.placements[i];
        if (std::tie(x.info, x.minPlayers, x.flags, x.name, x.params) !=
                std::tie(y.info, y.minPlayers, y.flags, y.name, y.params) ||
            !sameBits(x.position, y.position) || !sameBits(x.rotation, y.rotation) ||
            x.collision.size() != y.collision.size()) {
            return false;
        }
        for (usize triangle = 0; triangle < x.collision.size(); ++triangle) {
            const auto& left = x.collision[triangle];
            const auto& right = y.collision[triangle];
            if (!sameBits(left.normal, right.normal)) {
                return false;
            }
            for (usize vertex = 0; vertex < left.vertices.size(); ++vertex) {
                if (!sameBits(left.vertices[vertex], right.vertices[vertex])) {
                    return false;
                }
            }
        }
    }
    return true;
}
bool animation(AnimationPlayer& player, const TreeInfo& tree,
               const AnimationPlayer::Checkpoint& saved) {
    if (saved.playing && saved.sequence >= tree.sequences.size()) {
        return false;
    }
    return player.restore(saved, saved.playing ? &tree.sequences[saved.sequence] : nullptr);
}
bool particlesWithin(const ParticleField::Checkpoint& field, usize& count) {
    for (const auto& entry : field.entries) {
        if (entry.emitter.particles.size() > PlacedItemsCheckpoint::State::kMaxParticles - count) {
            return false;
        }
        count += entry.emitter.particles.size();
    }
    return true;
}
} // namespace

bool PlacedItemsCheckpoint::State::operator==(const State& other) const {
    return sameDefinitions(definitions, other.definitions) &&
           samePlacements(placements, other.placements) &&
           std::tie(archives, figures, bursts, items, effects, particles, particleResources,
                    textures, players, revealTime, revealing, capturePending, burstsAdvanced,
                    layout, collision) ==
               std::tie(other.archives, other.figures, other.bursts, other.items, other.effects,
                        other.particles, other.particleResources, other.textures, other.players,
                        other.revealTime, other.revealing, other.capturePending,
                        other.burstsAdvanced, other.layout, other.collision);
}

bool PlacedItemsCheckpoint::State::valid() const {
    return validIdentity() && validMutable();
}

bool PlacedItemsCheckpoint::State::validIdentity() const {
    if (definitions.size() > kMaxItems || placements.size() > kMaxItems ||
        figures.size() > kMaxResources || bursts.size() > kMaxResources ||
        archives.size() > kMaxArchives ||
        (!layout && (!definitions.empty() || !placements.empty()))) {
        return false;
    }
    usize choices = 0;
    for (const auto& info : definitions) {
        if (!name(info.name) || !scalar(info.radius) || !scalar(info.height) ||
            !scalar(info.xSize) || !scalar(info.zSize) || !vector(info.collisionOffset) ||
            info.choices.size() > kMaxItems - choices) {
            return false;
        }
        choices += info.choices.size();
    }
    usize triangles = 0;
    for (const auto& placement : placements) {
        if (!name(placement.name) || !vector(placement.position) || !vector(placement.rotation) ||
            placement.collision.size() > kMaxTriangles - triangles) {
            return false;
        }
        triangles += placement.collision.size();
        for (const auto& triangle : placement.collision) {
            if (!vector(triangle.normal) || !std::ranges::all_of(triangle.vertices, vector)) {
                return false;
            }
        }
    }
    for (const auto* roster : {&figures, &bursts}) {
        std::string previous;
        for (const auto& resource : *roster) {
            if (!name(resource.name) || resource.name.empty() || resource.name <= previous ||
                resource.archive >= archives.size() || !archives[resource.archive]) {
                return false;
            }
            previous = resource.name;
        }
    }
    return true;
}

bool PlacedItemsCheckpoint::State::validMutable() const {
    if (items.size() > kMaxItems || effects.size() > kMaxEffects ||
        textures.size() != static_cast<usize>(std::ranges::count(archives, true)) || players < 0 ||
        players > 4 || !scalar(revealTime) || revealTime < 0 ||
        particleResources.size() != particles.entries.size() || !particles.valid()) {
        return false;
    }
    if (!std::ranges::all_of(textures, [](const auto& value) { return value.valid(); })) {
        return false;
    }
    usize particleCount = 0;
    if (!particlesWithin(particles, particleCount)) {
        return false;
    }
    s32 lastInstance = -1;
    bool dynamic = false;
    for (const auto& item : items) {
        if (item.resource >= figures.size() || item.info < 0 ||
            static_cast<usize>(item.info) >= definitions.size() || item.instance < -1 ||
            item.opener < -1 || item.opener >= 4 || !scalar(item.strength) ||
            !scalar(item.radius) || item.radius < 0 || !scalar(item.height) || item.height < 0 ||
            !vector(item.position) || !matrix(item.transform) || !vector(item.velocity) ||
            !scalar(item.alpha) || item.alpha < 0 || item.alpha > 1 ||
            !scalar(item.noGrabSeconds) || item.noGrabSeconds < 0 ||
            !matrix(item.previousTransform) || !scalar(item.previousFrame) ||
            item.previousFrame < 0 || !scalar(item.previousAlpha) || item.previousAlpha < 0 ||
            item.previousAlpha > 1 || !item.animation.valid() || !item.pose.valid() ||
            !item.particles.valid() || !particlesWithin(item.particles.field, particleCount)) {
            return false;
        }
        const auto& definition = definitions[static_cast<usize>(item.info)];
        if (definition.type != ItemInfo::kPowerup || item.subtype != definition.subtype ||
            item.radius != definition.radius || item.height != definition.height ||
            item.flags != definition.properties) {
            return false;
        }
        if (item.instance >= 0) {
            if (dynamic || item.instance <= lastInstance ||
                static_cast<usize>(item.instance) >= placements.size()) {
                return false;
            }
            const auto& placement = placements[static_cast<usize>(item.instance)];
            if (placement.info != item.info || placement.minPlayers != item.minPlayers) {
                return false;
            }
            lastInstance = item.instance;
        } else {
            dynamic = true;
            if (item.minPlayers != 0) {
                return false;
            }
        }
        if (item.floor && (!collision || item.floor->object < 0 || item.floor->object > 65535 ||
                           !matrix(item.floor->local))) {
            return false;
        }
        if (item.presentationPose && (item.presentationPose->resource >= figures.size() ||
                                      !item.presentationPose->pose.valid())) {
            return false;
        }
    }
    std::vector<bool> used(particles.entries.size());
    for (const auto& effect : effects) {
        if (effect.resource >= bursts.size() || !vector(effect.position) ||
            !effect.animation.valid() || !effect.pose.valid() || !scalar(effect.secondsLeft) ||
            effect.emitters.empty() || effect.emitters.size() > TreePose::Checkpoint::kMaxNodes) {
            return false;
        }
        u32 previous = 0;
        bool first = true;
        for (const auto& [node, field] : effect.emitters) {
            if (node >= effect.pose.nodes.size() || field >= used.size() || used[field] ||
                (!first && node <= previous)) {
                return false;
            }
            used[field] = true;
            previous = node;
            first = false;
        }
    }
    for (usize i = 0; i < particles.entries.size(); ++i) {
        const auto& entry = particles.entries[i];
        if (entry.worldObject != -1 || particleResources[i] >= kMaxResources ||
            (!used[i] && (entry.emitter.phase != ParticleEmitter::Phase::Done ||
                          !entry.emitter.particles.empty()))) {
            return false;
        }
    }
    return true;
}

struct PlacedItemsCheckpoint::Catalog {
    struct Emitter {
        ParticleDescriptor descriptor;
        const Texture* texture = nullptr;
    };
    const WorldLayout* layout = nullptr;
    std::shared_ptr<const u8> binding;
    std::vector<ItemArchive*> archives;
    State identity;
    std::vector<s32> initialInstances;
    std::vector<PlacedItems::Item> figures;
    std::vector<PlacedItems::Effect> bursts;
    std::vector<std::vector<u32>> effectEmitters;
    std::vector<Emitter> emitters;
};
struct PlacedItemsCheckpoint::Prepared::Data {
    const PlacedItemsCheckpoint* wrapper = nullptr;
    const PlacedItems* owner = nullptr;
    const WorldCollision* collision = nullptr;
    std::shared_ptr<const u8> lifetime;
    std::shared_ptr<const Catalog> catalog;
    u64 generation = 0;
    State state;
    std::vector<PlacedItems::Item> items;
    std::vector<TreePose> presentedPoses;
    std::vector<PlacedItems::Effect> effects;
    std::vector<PlacedItems::ArchiveMotion> motions;
    ParticleField bursts;
};

PlacedItemsCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
PlacedItemsCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
PlacedItemsCheckpoint::Prepared&
PlacedItemsCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
PlacedItemsCheckpoint::Prepared::~Prepared() = default;
PlacedItemsCheckpoint::PlacedItemsCheckpoint(PlacedItems& owner) : m_owner(owner) {}
PlacedItemsCheckpoint::~PlacedItemsCheckpoint() = default;
void PlacedItemsCheckpoint::clearResources() {
    m_catalog.reset();
}

bool PlacedItemsCheckpoint::bindResources(RenderDevice& device) {
    clearResources();
    if (m_owner.m_archives.size() > State::kMaxArchives ||
        m_owner.m_infos.size() > State::kMaxItems ||
        (m_owner.m_layout && m_owner.m_layout->itemInstances().size() > State::kMaxItems)) {
        return false;
    }
    auto catalog = std::make_shared<Catalog>();
    catalog->layout = m_owner.m_layout;
    catalog->binding = m_owner.m_checkpointBinding;
    catalog->archives = m_owner.m_archives;
    auto& identity = catalog->identity;
    identity.layout = m_owner.m_layout != nullptr;
    identity.definitions = m_owner.m_infos;
    if (m_owner.m_layout) {
        identity.placements = m_owner.m_layout->itemInstances();
    }
    for (auto* archive : m_owner.m_archives) {
        identity.archives.push_back(archive && archive->loaded());
    }
    for (const auto& motion : m_owner.m_motions) {
        identity.textures.push_back(motion.animator.checkpoint());
    }
    std::set<std::string> names{"BADMEAT", "GAPPLE",  "TREAS_SILVER", "TREAS_GOLD",
                                "APPLE",   "CHICKEN", "TREAS_JUNK"};
    for (const auto& info : m_owner.m_infos) {
        if (info.type == ItemInfo::kPowerup && !info.name.empty()) {
            names.insert(info.name);
        }
    }
    for (const auto& placement : identity.placements) {
        if (placement.info >= 0 && static_cast<usize>(placement.info) < m_owner.m_infos.size() &&
            m_owner.m_infos[static_cast<usize>(placement.info)].type == ItemInfo::kPowerup &&
            !placement.name.empty()) {
            names.insert(placement.name);
        }
    }
    for (const auto& resourceName : names) {
        PlacedItems::Item item;
        item.name = resourceName;
        if (!m_owner.makeFigure(device, item)) {
            continue;
        }
        const auto found = std::ranges::find(catalog->archives, item.archive);
        if (found == catalog->archives.end() || catalog->figures.size() >= State::kMaxResources) {
            return false;
        }
        identity.figures.push_back(
            {resourceName, static_cast<u32>(found - catalog->archives.begin())});
        catalog->figures.push_back(std::move(item));
    }
    names.clear();
    for (const auto effect : PlacedItems::kGemEffects) {
        if (!effect.empty()) {
            names.emplace(effect);
        }
    }
    names.emplace(PlacedItems::kRuneEffect);
    names.emplace(PlacedItems::kGargoyleEffect);
    for (const auto& resourceName : names) {
        PlacedItems scratch;
        scratch.m_archives = catalog->archives;
        scratch.m_scope = AssetScope(catalog->archives);
        // Reuse native descriptor/texture fallback and node ordering, including
        // trees without mesh nodes. Missing resources remain unavailable, never fabricated.
        scratch.startEffect(device, resourceName, Vec3{0});
        if (scratch.m_sim.effects.empty()) {
            continue;
        }
        auto effect = std::move(scratch.m_sim.effects.front());
        usize archiveIndex = 0;
        for (; archiveIndex < catalog->archives.size(); ++archiveIndex) {
            auto* archive = catalog->archives[archiveIndex];
            const auto tree =
                archive && archive->loaded() ? archive->trees.find(resourceName) : std::nullopt;
            if (tree && &archive->trees.tree(*tree) == effect.figure) {
                break;
            }
        }
        if (archiveIndex == catalog->archives.size()) {
            return false;
        }
        identity.bursts.push_back({resourceName, static_cast<u32>(archiveIndex)});
        std::vector<u32> emitters;
        for (const auto& [node, field] : effect.emitters) {
            (void)node;
            const auto& descriptor = scratch.m_sim.bursts.emitter(field).descriptor();
            const auto* texture = scratch.m_sim.bursts.textureOf(field);
            auto found = std::ranges::find_if(catalog->emitters, [&](const auto& candidate) {
                return candidate.descriptor == descriptor && candidate.texture == texture;
            });
            if (found == catalog->emitters.end()) {
                if (catalog->emitters.size() >= State::kMaxResources) {
                    return false;
                }
                emitters.push_back(static_cast<u32>(catalog->emitters.size()));
                catalog->emitters.push_back({descriptor, texture});
            } else {
                emitters.push_back(static_cast<u32>(found - catalog->emitters.begin()));
            }
        }
        catalog->effectEmitters.push_back(std::move(emitters));
        catalog->bursts.push_back(std::move(effect));
    }
    for (const auto& item : m_owner.m_sim.items) {
        if (item.instance >= 0) {
            catalog->initialInstances.push_back(item.instance);
        }
    }
    if (!identity.valid()) {
        return false;
    }
    m_catalog = std::move(catalog);
    if (!checkpoint()) {
        clearResources();
        return false;
    }
    return true;
}

bool PlacedItemsCheckpoint::ready() const {
    if (!m_catalog || m_catalog->layout != m_owner.m_layout ||
        m_catalog->binding != m_owner.m_checkpointBinding ||
        m_catalog->archives != m_owner.m_archives) {
        return false;
    }
    for (usize i = 0; i < m_catalog->archives.size(); ++i) {
        const auto* archive = m_catalog->archives[i];
        if ((archive && archive->loaded()) != m_catalog->identity.archives[i]) {
            return false;
        }
    }
    for (usize i = 0; i < m_catalog->figures.size(); ++i) {
        const auto& item = m_catalog->figures[i];
        const auto found = item.archive->trees.find(item.name);
        if (!found || &item.archive->trees.tree(*found) != item.figure) {
            return false;
        }
    }
    for (usize i = 0; i < m_catalog->bursts.size(); ++i) {
        const auto& resource = m_catalog->identity.bursts[i];
        const auto* archive = m_catalog->archives[resource.archive];
        const auto found = archive->trees.find(resource.name);
        if (!found || &archive->trees.tree(*found) != m_catalog->bursts[i].figure) {
            return false;
        }
    }
    return true;
}

bool PlacedItemsCheckpoint::placementValid(const PlacedItems::Item& item) const {
    if (!vector(item.position) || !matrix(item.transform) || !vector(item.velocity) ||
        !scalar(item.strength) || !scalar(item.noGrabSeconds) || item.noGrabSeconds < 0) {
        return false;
    }
    return !item.floor || (m_owner.m_collision && item.floor->object >= 0 &&
                           item.floor->object <= 65535 && matrix(item.floor->local) &&
                           m_owner.m_collision->objectTransform(item.floor->object).has_value());
}

std::optional<PlacedItems::Item> PlacedItemsCheckpoint::birth(s32 record, const Vec3& position,
                                                              const WorldCollision* collision,
                                                              s32 amount) const {
    if (!ready() || m_owner.m_sim.items.size() >= State::kMaxItems || !vector(position) ||
        (collision && collision != m_owner.m_collision) || record < 0 ||
        static_cast<usize>(record) >= m_owner.m_infos.size()) {
        return std::nullopt;
    }
    const auto& info = m_owner.m_infos[static_cast<usize>(record)];
    if (info.type != ItemInfo::kPowerup) {
        return std::nullopt;
    }
    const auto prototype =
        std::ranges::find(m_catalog->figures, info.name, &PlacedItems::Item::name);
    if (prototype == m_catalog->figures.end()) {
        return std::nullopt;
    }
    const auto pose = prototype->pose.checkpoint();
    const auto particles = prototype->particles.checkpoint();
    if (!pose || !pose->valid() || !prototype->player.checkpoint().valid() || !particles.valid() ||
        std::ranges::any_of(particles.field.entries,
                            [](const auto& entry) { return !entry.emitter.particles.empty(); })) {
        return std::nullopt;
    }
    // makeFigure binds at the default placement before native floor/throw rules.
    // TreeParticles seeds each authored node with i+1, independent of the item
    // index or sorted catalog index. The immutable prototype retains that exact
    // empty birth state; neither a live bank clock nor an older item is cloned.
    auto item = *prototype;
    PlacedItems::setRecord(item, info, record, amount);
    PlacedItems::placeAt(item, position, collision);
    return placementValid(item) ? std::optional{std::move(item)} : std::nullopt;
}

std::optional<usize> PlacedItemsCheckpoint::append(PlacedItems::Item item) {
    if (!placementValid(item)) {
        return std::nullopt;
    }
    // All fallible CPU preparation precedes the vector's strongly exception-safe
    // insertion. Resource/generation fences are unchanged by native gameplay.
    static_assert(std::is_nothrow_move_constructible_v<PlacedItems::Item>);
    const usize index = m_owner.m_sim.items.size();
    m_owner.m_sim.items.push_back(std::move(item));
    return index;
}

std::optional<usize> PlacedItemsCheckpoint::place(std::string_view itemName, const Vec3& position,
                                                  const WorldCollision* collision) {
    const auto record = m_owner.recordOf(itemName);
    return record ? placeRecord(*record, position, collision) : std::nullopt;
}

std::optional<usize> PlacedItemsCheckpoint::placeRecord(s32 record, const Vec3& position,
                                                        const WorldCollision* collision,
                                                        s32 amount) {
    auto item = birth(record, position, collision, amount);
    return item ? append(std::move(*item)) : std::nullopt;
}

std::optional<usize> PlacedItemsCheckpoint::throwItem(std::string_view itemName,
                                                      const Vec3& position, const Vec3& velocity,
                                                      const WorldCollision* collision,
                                                      f32 noGrabSeconds,
                                                      std::optional<f32> strength) {
    if (!vector(velocity) || !scalar(noGrabSeconds) || noGrabSeconds < 0 ||
        (strength && !scalar(*strength))) {
        return std::nullopt;
    }
    const auto record = m_owner.recordOf(itemName);
    auto item = record ? birth(*record, position, collision, 0) : std::nullopt;
    if (!item) {
        return std::nullopt;
    }
    PlacedItems::throwFrom(*item, position, velocity, collision, noGrabSeconds, strength);
    return append(std::move(*item));
}

std::optional<bool> PlacedItemsCheckpoint::release(usize index, const Vec3& position,
                                                   const Vec3& velocity,
                                                   const WorldCollision* collision,
                                                   f32 noGrabSeconds) {
    if (!ready() || (collision && collision != m_owner.m_collision)) {
        return std::nullopt;
    }
    if (index >= m_owner.m_sim.items.size() || !m_owner.m_sim.items[index].carried) {
        return false;
    }
    if (!vector(position) || !vector(velocity) || !scalar(noGrabSeconds) || noGrabSeconds < 0) {
        return std::nullopt;
    }
    auto item = m_owner.m_sim.items[index];
    PlacedItems::releaseFrom(item, position, velocity, collision, noGrabSeconds, m_owner.m_sim.players);
    if (!placementValid(item)) {
        return std::nullopt;
    }
    static_assert(std::is_nothrow_move_assignable_v<PlacedItems::Item>);
    m_owner.m_sim.items[index] = std::move(item);
    return true;
}

std::optional<std::vector<Pickup>>
PlacedItemsCheckpoint::collect(std::span<const Collector> collectors,
                               const StagedPickupJudge& judge) {
    if (!ready() || !judge || m_owner.m_sim.items.size() > State::kMaxItems) {
        return std::nullopt;
    }
    return m_owner.collectUsing(collectors, judge, {});
}

std::optional<PlacedItemsCheckpoint::State> PlacedItemsCheckpoint::checkpoint() const {
    if (!ready() || m_owner.m_sim.items.size() > State::kMaxItems ||
        m_owner.m_sim.effects.size() > State::kMaxEffects) {
        return std::nullopt;
    }
    return capture(m_catalog->identity);
}

std::optional<PlacedItemsCheckpoint::State>
PlacedItemsCheckpoint::checkpoint(State&& reusable) const {
    if (!ready() || m_owner.m_sim.items.size() > State::kMaxItems ||
        m_owner.m_sim.effects.size() > State::kMaxEffects ||
        !sameCaptureIdentity(reusable, m_catalog->identity)) {
        return std::nullopt;
    }
    return capture(std::move(reusable));
}

std::optional<PlacedItemsCheckpoint::State> PlacedItemsCheckpoint::capture(State state) const {
    // Only immutable, catalog-verified storage survives. A caller's stale or
    // malformed live fields are never an input to native capture or validation.
    state.items.clear();
    state.effects.clear();
    state.particleResources.clear();
    state.items.reserve(m_owner.m_sim.items.size());
    state.effects.reserve(m_owner.m_sim.effects.size());
    state.players = m_owner.m_sim.players;
    state.revealTime = m_owner.m_sim.revealTime;
    state.revealing = m_owner.m_sim.revealing;
    state.capturePending = m_owner.m_sim.capturePending;
    state.burstsAdvanced = m_owner.m_sim.burstsAdvanced;
    state.collision = m_owner.m_collision != nullptr;
    state.textures.clear();
    for (const auto& motion : m_owner.m_motions) {
        state.textures.push_back(motion.animator.checkpoint());
    }
    for (const auto& item : m_owner.m_sim.items) {
        const auto resource = std::ranges::find_if(m_catalog->figures, [&](const auto& prototype) {
            return prototype.figure == item.figure && prototype.archive == item.archive &&
                   prototype.name == item.name;
        });
        auto pose = item.pose.checkpoint();
        if (resource == m_catalog->figures.end() || !pose) {
            return std::nullopt;
        }
        Item saved;
        saved.resource = static_cast<u32>(resource - m_catalog->figures.begin());
        if (item.floor) {
            saved.floor = Floor{item.floor->object, item.floor->local};
        }
        saved.instance = item.instance;
        saved.info = item.info;
        saved.subtype = item.subtype;
        saved.value = item.value;
        saved.health = item.health;
        saved.flags = item.flags;
        saved.strength = item.strength;
        saved.minPlayers = item.minPlayers;
        saved.radius = item.radius;
        saved.height = item.height;
        saved.position = item.position;
        saved.transform = item.transform;
        saved.particles = item.particles.checkpoint();
        saved.pose = std::move(*pose);
        saved.animation = item.player.checkpoint();
        saved.alpha = item.alpha;
        saved.visible = item.visible;
        saved.taken = item.taken;
        saved.contained = item.contained;
        saved.velocity = item.velocity;
        saved.thrown = item.thrown;
        saved.carried = item.carried;
        saved.opener = item.opener;
        saved.noGrabSeconds = item.noGrabSeconds;
        saved.previousTransform = item.previousTransform;
        saved.previousFrame = item.previousFrame;
        saved.previousAlpha = item.previousAlpha;
        saved.previousGeneration = item.previousGeneration;
        saved.presentationCaptured = item.presentationCaptured;
        saved.continuity = item.continuity;
        const auto* presented = m_owner.presentationPoseOf(state.items.size());
        if (presented != nullptr && presented->posed()) {
            auto presentation = presented->checkpoint();
            if (!presentation) {
                return std::nullopt;
            }
            for (usize i = 0; i < m_catalog->figures.size(); ++i) {
                const auto& tree = *m_catalog->figures[i].figure;
                // Reject impossible identities before restore rescans the pose.
                // Compatible candidates still validate topology and composed
                // matrices in their original first-success catalog order.
                if (tree.name != presentation->tree ||
                    tree.nodes.size() != presentation->nodes.size()) {
                    continue;
                }
                TreePose candidate;
                if (candidate.restore(tree, *presentation)) {
                    saved.presentationPose = Pose{static_cast<u32>(i), std::move(*presentation)};
                    break;
                }
            }
            if (!saved.presentationPose) {
                return std::nullopt;
            }
        }
        state.items.push_back(std::move(saved));
    }
    state.particles = m_owner.m_sim.bursts.checkpoint();
    for (usize i = 0; i < state.particles.entries.size(); ++i) {
        const auto& descriptor = state.particles.entries[i].emitter.descriptor;
        const auto found = std::ranges::find_if(m_catalog->emitters, [&](const auto& resource) {
            return resource.descriptor == descriptor &&
                   resource.texture == m_owner.m_sim.bursts.textureOf(i);
        });
        if (found == m_catalog->emitters.end()) {
            return std::nullopt;
        }
        state.particleResources.push_back(static_cast<u32>(found - m_catalog->emitters.begin()));
    }
    for (const auto& effect : m_owner.m_sim.effects) {
        const auto found = std::ranges::find_if(m_catalog->bursts, [&](const auto& prototype) {
            return prototype.figure == effect.figure;
        });
        auto pose = effect.pose.checkpoint();
        if (found == m_catalog->bursts.end() || !pose) {
            return std::nullopt;
        }
        Effect saved{static_cast<u32>(found - m_catalog->bursts.begin()),
                     effect.position,
                     effect.player.checkpoint(),
                     std::move(*pose),
                     {},
                     effect.secondsLeft,
                     effect.emitting};
        for (const auto& [node, field] : effect.emitters) {
            if (node > std::numeric_limits<u32>::max() || field > std::numeric_limits<u32>::max()) {
                return std::nullopt;
            }
            saved.emitters.emplace_back(static_cast<u32>(node), static_cast<u32>(field));
        }
        state.effects.push_back(std::move(saved));
    }
    // The identity is copied from or exactly compared with the immutable catalog
    // that bindResources validated. Live values and their catalog links are
    // checked on every capture; incoming states still require full validation.
    return state.validMutable() ? std::optional{std::move(state)} : std::nullopt;
}

std::optional<PlacedItemsCheckpoint::Prepared>
PlacedItemsCheckpoint::prepareRestore(const State& state) const {
    if (!ready() || !state.valid() || state.layout != m_catalog->identity.layout ||
        state.collision != (m_owner.m_collision != nullptr) ||
        state.archives != m_catalog->identity.archives ||
        state.figures != m_catalog->identity.figures ||
        state.bursts != m_catalog->identity.bursts ||
        !sameDefinitions(state.definitions, m_catalog->identity.definitions) ||
        !samePlacements(state.placements, m_catalog->identity.placements) ||
        state.textures.size() != m_owner.m_motions.size() ||
        m_owner.m_checkpointGeneration == std::numeric_limits<u64>::max()) {
        return std::nullopt;
    }
    std::vector<s32> initial;
    for (const auto& item : state.items) {
        if (item.instance >= 0) {
            initial.push_back(item.instance);
        }
    }
    if (initial != m_catalog->initialInstances) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->wrapper = this;
    data->owner = &m_owner;
    data->collision = m_owner.m_collision;
    data->lifetime = m_lifetime;
    data->catalog = m_catalog;
    data->generation = m_owner.m_checkpointGeneration;
    data->state = state;
    for (const auto& saved : state.items) {
        auto item = m_catalog->figures[saved.resource];
        item.model = std::make_shared<TreeModel>(*item.model);
        if (!animation(item.player, *item.figure, saved.animation) ||
            !item.pose.restore(*item.figure, saved.pose) ||
            !item.particles.restore(saved.particles)) {
            return std::nullopt;
        }
        if (saved.floor) {
            if (!m_owner.m_collision ||
                !m_owner.m_collision->objectTransform(saved.floor->object)) {
                return std::nullopt;
            }
            item.floor = PlacedItems::Item::Floor{saved.floor->object, saved.floor->local};
        }
        item.instance = saved.instance;
        item.info = saved.info;
        item.subtype = saved.subtype;
        item.value = saved.value;
        item.health = saved.health;
        item.flags = saved.flags;
        item.strength = saved.strength;
        item.minPlayers = saved.minPlayers;
        item.radius = saved.radius;
        item.height = saved.height;
        item.position = saved.position;
        item.transform = saved.transform;
        item.alpha = saved.alpha;
        item.visible = saved.visible;
        item.taken = saved.taken;
        item.contained = saved.contained;
        item.velocity = saved.velocity;
        item.thrown = saved.thrown;
        item.carried = saved.carried;
        item.opener = saved.opener;
        item.noGrabSeconds = saved.noGrabSeconds;
        item.previousTransform = saved.previousTransform;
        item.previousFrame = saved.previousFrame;
        item.previousAlpha = saved.previousAlpha;
        item.previousGeneration = saved.previousGeneration;
        item.presentationCaptured = saved.presentationCaptured;
        item.continuity = saved.continuity;
        auto& presented = data->presentedPoses.emplace_back();
        if (saved.presentationPose &&
            !presented.restore(
                *m_catalog->figures[saved.presentationPose->resource].figure,
                saved.presentationPose->pose)) {
            return std::nullopt;
        }
        if (item.player.playing()) {
            item.model->setFrame(item.player.sequence(), static_cast<s32>(item.player.frame()));
        }
        data->items.push_back(std::move(item));
    }
    for (usize i = 0; i < state.particles.entries.size(); ++i) {
        const auto id = state.particleResources[i];
        if (id >= m_catalog->emitters.size()) {
            return std::nullopt;
        }
        const auto& resource = m_catalog->emitters[id];
        data->bursts.start(resource.descriptor, Mat4{1}, resource.texture);
    }
    if (!data->bursts.restore(state.particles)) {
        return std::nullopt;
    }
    for (const auto& saved : state.effects) {
        auto effect = m_catalog->bursts[saved.resource];
        if (!animation(effect.player, *effect.figure, saved.animation) ||
            !effect.pose.restore(*effect.figure, saved.pose) ||
            saved.emitters.size() != effect.emitters.size()) {
            return std::nullopt;
        }
        for (usize i = 0; i < saved.emitters.size(); ++i) {
            const auto [node, field] = saved.emitters[i];
            if (node != effect.emitters[i].first ||
                state.particleResources[field] != m_catalog->effectEmitters[saved.resource][i]) {
                return std::nullopt;
            }
            effect.emitters[i] = {node, field};
        }
        effect.position = saved.position;
        effect.secondsLeft = saved.secondsLeft;
        effect.emitting = saved.emitting;
        data->effects.push_back(std::move(effect));
    }
    data->motions = m_owner.m_motions;
    for (usize i = 0; i < data->motions.size(); ++i) {
        if (!data->motions[i].animator.restore(state.textures[i])) {
            return std::nullopt;
        }
    }
    // Restore native texture bindings on every visible item before publication.
    for (auto& item : data->items) {
        if (!item.visible) {
            continue;
        }
        for (const auto& motion : data->motions) {
            if (motion.archive == item.archive) {
                motion.animator.apply(*item.model, *item.figure, item.player.sequence(),
                                      static_cast<s32>(item.player.frame()));
                motion.animator.apply(item.particles, *item.figure, item.player.sequence(),
                                      static_cast<s32>(item.player.frame()));
            }
        }
    }
    Prepared prepared{std::move(data)};
    return canCommit(prepared) ? std::optional{std::move(prepared)} : std::nullopt;
}

bool PlacedItemsCheckpoint::canCommit(const Prepared& prepared) const {
    if (!prepared.m_data || !ready()) {
        return false;
    }
    const auto& data = *prepared.m_data;
    if (data.wrapper != this || data.owner != &m_owner || data.lifetime != m_lifetime ||
        data.catalog != m_catalog || data.collision != m_owner.m_collision ||
        data.generation != m_owner.m_checkpointGeneration ||
        data.generation == std::numeric_limits<u64>::max()) {
        return false;
    }
    for (const auto& item : data.state.items) {
        if (item.floor &&
            (!m_owner.m_collision || !m_owner.m_collision->objectTransform(item.floor->object))) {
            return false;
        }
    }
    return true;
}
bool PlacedItemsCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    m_owner.m_sim.items.swap(data->items);
    m_owner.m_presentedPoses.swap(data->presentedPoses);
    m_owner.m_sim.effects.swap(data->effects);
    m_owner.m_motions.swap(data->motions);
    std::swap(m_owner.m_sim.bursts, data->bursts);
    m_owner.m_sim.players = data->state.players;
    m_owner.m_sim.revealTime = data->state.revealTime;
    m_owner.m_sim.revealing = data->state.revealing;
    m_owner.m_sim.capturePending = data->state.capturePending;
    m_owner.m_sim.burstsAdvanced = data->state.burstsAdvanced;
    ++m_owner.m_checkpointGeneration;
    return true;
}
bool PlacedItemsCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl::game
