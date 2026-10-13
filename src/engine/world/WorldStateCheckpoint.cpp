#include "engine/world/WorldStateCheckpoint.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

namespace gdl {
namespace {
constexpr f32 kMagnitude = 1'000'000;
bool scalar(f32 value) {
    return std::isfinite(value) && std::abs(value) <= kMagnitude;
}
bool vector(const Vec3& value) {
    return scalar(value.x) && scalar(value.y) && scalar(value.z);
}
bool vector2(const Vec2& value) {
    return scalar(value.x) && scalar(value.y);
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
WorldStateCheckpoint::Triangle triangle(const CollisionTriangle& value) {
    return {value.normal, value.vertices, value.object, value.objectFlags};
}
std::vector<WorldStateCheckpoint::Triangle> triangles(std::span<const CollisionTriangle> source) {
    std::vector<WorldStateCheckpoint::Triangle> result;
    result.reserve(source.size());
    for (const auto& value : source) {
        result.push_back(triangle(value));
    }
    return result;
}
bool sameTriangles(std::span<const CollisionTriangle> source,
                   std::span<const WorldStateCheckpoint::Triangle> state) {
    if (source.size() != state.size()) {
        return false;
    }
    for (usize i = 0; i < source.size(); ++i) {
        if (triangle(source[i]) != state[i]) {
            return false;
        }
    }
    return true;
}
bool validTriangles(std::span<const WorldStateCheckpoint::Triangle> values, usize& count) {
    if (values.size() > WorldStateCheckpoint::State::kMaxTriangles - count) {
        return false;
    }
    count += values.size();
    return std::ranges::all_of(values, [](const auto& value) {
        return value.object >= -1 && value.object <= 1'000'000 && vector(value.normal) &&
               std::ranges::all_of(value.vertices, vector);
    });
}
bool mask(const std::vector<s32>& values, s32 minimum) {
    if (values.size() > WorldStateCheckpoint::State::kMaxObjects) {
        return false;
    }
    std::set<s32> unique;
    for (const auto value : values) {
        if (value < minimum || value > 1'000'000 || !unique.insert(value).second) {
            return false;
        }
    }
    return true;
}
} // namespace

bool WorldStateCheckpoint::Light::valid() const {
    return vector(ambient) && vector(color) && vector(direction) && points.size() <= 4096 &&
           std::ranges::all_of(points, [](const Point& point) {
               return vector(point.position) && vector(point.color) && scalar(point.radius) &&
                      point.radius >= 0 && scalar(point.intensity);
           });
}
WorldStateCheckpoint::Light WorldStateCheckpoint::Light::capture(const WorldLighting& source) {
    Light result{source.ambient, source.lightColor, source.direction, {}};
    for (const auto& point : source.points) {
        result.points.push_back({point.position, point.color, point.radius, point.intensity});
    }
    return result;
}
WorldLighting WorldStateCheckpoint::Light::lighting() const {
    WorldLighting result;
    result.ambient = ambient;
    result.lightColor = color;
    result.direction = direction;
    for (const auto& point : points) {
        result.points.push_back({point.position, point.color, point.radius, point.intensity});
    }
    return result;
}
bool WorldStateCheckpoint::State::valid() const {
    if (resources.size() > kMaxTextures || slots.size() > kMaxTextures ||
        placements.size() > kMaxObjects || units.size() > placements.size() ||
        moving.size() > kMaxObjects || animations.size() > kMaxAnimations ||
        cycles.size() > kMaxEvents || !light.valid() || !scalar(darken) || !mask(hidden, -1) ||
        !mask(contactOnly, -1) || !mask(blockedFloorExits, 0)) {
        return false;
    }
    std::set<std::tuple<Resource::Kind, u32, u32>> keys;
    for (const auto& resource : resources) {
        if (resource.kind > Resource::Kind::Cycle || resource.index >= kMaxTextures ||
            resource.frame >= kMaxTextures ||
            (resource.kind == Resource::Kind::Slot && resource.frame != 0) ||
            !keys.emplace(resource.kind, resource.index, resource.frame).second) {
            return false;
        }
    }
    for (usize i = 0; i < placements.size(); ++i) {
        const auto& placement = placements[i];
        if (!matrix(placement.local) || !matrix(placement.previous) || !matrix(placement.initial) ||
            placement.parent < -1 || placement.parent >= static_cast<s32>(placements.size()) ||
            placement.parent == static_cast<s32>(i) || placement.unit < -1 ||
            placement.unit >= static_cast<s32>(units.size()) ||
            (placement.unit >= 0 && units[static_cast<usize>(placement.unit)].object != i)) {
            return false;
        }
    }
    std::set<u32> unitObjects;
    for (usize i = 0; i < units.size(); ++i) {
        const auto& unit = units[i];
        if (unit.object >= placements.size() ||
            placements[unit.object].unit != static_cast<s32>(i) ||
            !unitObjects.insert(unit.object).second || !scalar(unit.alpha) || unit.alpha < 0 ||
            unit.alpha > 1) {
            return false;
        }
    }
    usize cyclesSize = 0;
    u32 previous = 0;
    bool first = true;
    const auto resourceValid = [&](const std::optional<u32>& id) {
        return !id || *id < resources.size();
    };
    for (const auto& slot : slots) {
        if (slot.index >= kMaxTextures || (!first && slot.index <= previous) ||
            !resourceValid(slot.texture) || !resourceValid(slot.frame) ||
            slot.cycle.size() > kMaxTextures - cyclesSize || !scalar(slot.cyclePosition) ||
            !scalar(slot.cycleSpeed) || !vector2(slot.offset) || !vector2(slot.scrollPhase) ||
            !vector2(slot.scrollVelocity)) {
            return false;
        }
        cyclesSize += slot.cycle.size();
        for (const auto id : slot.cycle) {
            if (id >= resources.size()) {
                return false;
            }
        }
        previous = slot.index;
        first = false;
    }
    usize totalTriangles = 0;
    if (!validTriangles(triangles, totalTriangles)) {
        return false;
    }
    std::set<s32> movers;
    for (const auto& object : moving) {
        if (object.object < 0 || object.object > 1'000'000 ||
            !movers.insert(object.object).second || object.local.size() != object.placed.size() ||
            !validTriangles(object.local, totalTriangles) ||
            !validTriangles(object.placed, totalTriangles) || !matrix(object.transform) ||
            !vector(object.minimum) || !vector(object.maximum)) {
            return false;
        }
    }
    for (const auto& cursor : animations) {
        if (!cursor.valid() || static_cast<usize>(cursor.object) >= placements.size()) {
            return false;
        }
    }
    for (const auto& event : cycles) {
        if (!std::ranges::any_of(
                animations, [&](const auto& cursor) { return cursor.object == event.object; })) {
            return false;
        }
    }
    return true;
}

struct WorldStateCheckpoint::Catalog {
    std::shared_ptr<const u8> binding;
    const TextureAnimator* textures = nullptr;
    std::vector<Resource> resources;
    std::vector<const Texture*> pointers;
    std::vector<u32> slots;
    std::vector<const WorldScene::Slot*> slotPointers;
    const WorldScene::Placement* placements = nullptr;
    const WorldScene::Unit* units = nullptr;
    const WorldScene::Batch* batches = nullptr;
    const WorldAnimator::Track* tracks = nullptr;
    const CollisionTriangle* staticTriangles = nullptr;
    const WorldCollision::MovingObject* moving = nullptr;
    const std::vector<u32>* cells = nullptr;
    usize placementCount = 0, unitCount = 0, batchCount = 0, trackCount = 0, staticCount = 0,
          movingCount = 0, cellCount = 0;
    u64 layout = 0;
    std::optional<u32> id(const Texture* texture) const {
        const auto found = std::ranges::find(pointers, texture);
        return found == pointers.end() ? std::nullopt
                                       : std::optional{static_cast<u32>(found - pointers.begin())};
    }
};
struct WorldStateCheckpoint::Prepared::Data {
    struct Slot {
        WorldScene::Slot* target = nullptr;
        const Texture* frame = nullptr;
        std::vector<const Texture*> cycle;
    };
    const WorldStateCheckpoint* wrapper = nullptr;
    std::shared_ptr<const u8> lifetime;
    std::shared_ptr<const Catalog> catalog;
    u64 generation = 0;
    State state;
    std::vector<Slot> slots;
    std::vector<WorldCollision::MovingObject> moving;
    std::vector<WorldAnimator::CycleEvent> events;
    WorldLighting lighting;
};

WorldStateCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
WorldStateCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
WorldStateCheckpoint::Prepared&
WorldStateCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
WorldStateCheckpoint::Prepared::~Prepared() = default;
WorldStateCheckpoint::WorldStateCheckpoint(WorldScene& scene, WorldCollision& collision,
                                           WorldAnimator& animator,
                                           const std::shared_ptr<const u8>& binding)
    : m_scene(scene), m_collision(collision), m_animator(animator), m_binding(binding) {}
WorldStateCheckpoint::~WorldStateCheckpoint() = default;
void WorldStateCheckpoint::clear() {
    m_catalog.reset();
}

bool WorldStateCheckpoint::bind(const TextureAnimator& textures) {
    clear();
    if (!m_binding || m_scene.m_slots.size() > State::kMaxTextures ||
        textures.size() > State::kMaxTextures || m_scene.m_placements.size() > State::kMaxObjects ||
        m_animator.size() > State::kMaxAnimations) {
        return false;
    }
    auto catalog = std::make_shared<Catalog>();
    catalog->binding = m_binding;
    catalog->textures = &textures;
    catalog->layout = m_scene.m_layoutSignature;
    for (const auto& [index, slot] : m_scene.m_slots) {
        (void)slot;
        catalog->slots.push_back(index);
    }
    std::ranges::sort(catalog->slots);
    const auto add = [&](const Texture* texture, Resource resource) {
        if (texture && !catalog->id(texture)) {
            catalog->resources.push_back(resource);
            catalog->pointers.push_back(texture);
        }
    };
    for (const auto index : catalog->slots) {
        const auto& slot = m_scene.m_slots.at(index);
        catalog->slotPointers.push_back(&slot);
        add(slot.texture, {Resource::Kind::Slot, index, 0});
    }
    for (usize index = 0; index < textures.size(); ++index) {
        usize frame = 0;
        for (; frame < State::kMaxTextures; ++frame) {
            const auto* texture = textures.cycleFrame(index, frame);
            if (!texture) {
                break;
            }
            add(texture, {Resource::Kind::Cycle, static_cast<u32>(index), static_cast<u32>(frame)});
        }
        if (frame == State::kMaxTextures && textures.cycleFrame(index, frame)) {
            return false;
        }
    }
    if (catalog->resources.size() > State::kMaxTextures) {
        return false;
    }
    catalog->placements = m_scene.m_placements.data();
    catalog->placementCount = m_scene.m_placements.size();
    catalog->units = m_scene.m_units.data();
    catalog->unitCount = m_scene.m_units.size();
    catalog->batches = m_scene.m_batches.data();
    catalog->batchCount = m_scene.m_batches.size();
    catalog->tracks = m_animator.m_tracks.data();
    catalog->trackCount = m_animator.m_tracks.size();
    catalog->staticTriangles = m_collision.m_triangles.data();
    catalog->staticCount = m_collision.m_triangles.size();
    catalog->moving = m_collision.m_moving.data();
    catalog->movingCount = m_collision.m_moving.size();
    catalog->cells = m_collision.m_cells.data();
    catalog->cellCount = m_collision.m_cells.size();
    m_catalog = std::move(catalog);
    if (!checkpoint()) {
        clear();
        return false;
    }
    return true;
}

bool WorldStateCheckpoint::ready() const {
    if (!m_catalog || m_catalog->binding != m_binding ||
        m_catalog->layout != m_scene.m_layoutSignature ||
        m_catalog->placements != m_scene.m_placements.data() ||
        m_catalog->placementCount != m_scene.m_placements.size() ||
        m_catalog->units != m_scene.m_units.data() ||
        m_catalog->unitCount != m_scene.m_units.size() ||
        m_catalog->batches != m_scene.m_batches.data() ||
        m_catalog->batchCount != m_scene.m_batches.size() ||
        m_catalog->tracks != m_animator.m_tracks.data() ||
        m_catalog->trackCount != m_animator.m_tracks.size() ||
        m_catalog->staticTriangles != m_collision.m_triangles.data() ||
        m_catalog->staticCount != m_collision.m_triangles.size() ||
        m_catalog->moving != m_collision.m_moving.data() ||
        m_catalog->movingCount != m_collision.m_moving.size() ||
        m_catalog->cells != m_collision.m_cells.data() ||
        m_catalog->cellCount != m_collision.m_cells.size() ||
        m_catalog->slots.size() != m_scene.m_slots.size()) {
        return false;
    }
    for (usize i = 0; i < m_catalog->slots.size(); ++i) {
        const auto found = m_scene.m_slots.find(m_catalog->slots[i]);
        if (found == m_scene.m_slots.end() || &found->second != m_catalog->slotPointers[i]) {
            return false;
        }
    }
    for (usize i = 0; i < m_catalog->resources.size(); ++i) {
        const auto& key = m_catalog->resources[i];
        const auto* actual = key.kind == Resource::Kind::Slot
                                 ? m_scene.m_slots.at(key.index).texture
                                 : m_catalog->textures->cycleFrame(key.index, key.frame);
        if (actual != m_catalog->pointers[i]) {
            return false;
        }
    }
    return true;
}

std::optional<WorldStateCheckpoint::State> WorldStateCheckpoint::checkpoint() const {
    if (!ready()) {
        return std::nullopt;
    }
    State state;
    state.layout = m_scene.m_layoutSignature;
    state.resources = m_catalog->resources;
    for (const auto& placement : m_scene.m_placements) {
        state.placements.push_back({placement.local, placement.initial, placement.previous,
                                    placement.parent, placement.unit, placement.continuity,
                                    placement.moving});
    }
    for (const auto& unit : m_scene.m_units) {
        state.units.push_back({static_cast<u32>(unit.object), unit.alpha, unit.visible});
    }
    for (const auto index : m_catalog->slots) {
        const auto& source = m_scene.m_slots.at(index);
        Slot slot{index,
                  m_catalog->id(source.texture),
                  m_catalog->id(source.frame),
                  {},
                  source.cyclePosition,
                  source.cycleSpeed,
                  source.offset,
                  source.scrollPhase,
                  source.scrollVelocity,
                  source.usable,
                  source.thermal};
        if ((source.texture && !slot.texture) || (source.frame && !slot.frame)) {
            return std::nullopt;
        }
        for (const auto* frame : source.cycle) {
            const auto id = m_catalog->id(frame);
            if (!id) {
                return std::nullopt;
            }
            slot.cycle.push_back(*id);
        }
        state.slots.push_back(std::move(slot));
    }
    state.light = Light::capture(m_scene.m_lighting);
    state.darken = m_scene.m_darken;
    state.textureFrame = m_scene.m_textureFrame;
    state.triangles = triangles(m_collision.m_triangles);
    for (const auto& object : m_collision.m_moving) {
        state.moving.push_back({object.object, triangles(object.local), triangles(object.placed),
                                object.boundsMin, object.boundsMax, object.world});
    }
    state.hidden = m_collision.m_hidden;
    state.contactOnly = m_collision.m_contactOnly;
    state.blockedFloorExits = m_collision.m_blockedFloorExits;
    state.animations = m_animator.checkpoint();
    for (const auto& event : m_animator.m_cycleEvents) {
        state.cycles.push_back({event.object, event.wrapped});
    }
    if (!state.valid()) {
        return std::nullopt;
    }
    return state;
}

std::optional<WorldStateCheckpoint::Prepared>
WorldStateCheckpoint::prepareRestore(const State& state) const {
    if (!ready() || !state.valid() || state.layout != m_catalog->layout ||
        state.resources != m_catalog->resources ||
        state.placements.size() != m_scene.m_placements.size() ||
        state.units.size() != m_scene.m_units.size() ||
        state.slots.size() != m_catalog->slots.size() ||
        state.moving.size() != m_collision.m_moving.size() ||
        state.animations.size() != m_animator.size() ||
        !sameTriangles(m_collision.m_triangles, state.triangles) ||
        m_generation == std::numeric_limits<u64>::max()) {
        return std::nullopt;
    }
    for (usize i = 0; i < state.placements.size(); ++i) {
        const auto& saved = state.placements[i];
        const auto& local = m_scene.m_placements[i];
        if (saved.initial != local.initial || saved.parent != local.parent ||
            saved.unit != local.unit || saved.moving != local.moving ||
            (!local.moving && saved.local != local.local)) {
            return std::nullopt;
        }
    }
    for (usize i = 0; i < state.units.size(); ++i) {
        if (state.units[i].object != m_scene.m_units[i].object) {
            return std::nullopt;
        }
    }
    for (usize i = 0; i < state.animations.size(); ++i) {
        if (state.animations[i].object != m_animator.m_tracks[i].object ||
            state.animations[i].frames != m_animator.m_tracks[i].frames) {
            return std::nullopt;
        }
    }
    auto data = std::make_unique<Prepared::Data>();
    data->wrapper = this;
    data->lifetime = m_lifetime;
    data->catalog = m_catalog;
    data->generation = m_generation;
    data->state = state;
    data->lighting = state.light.lighting();
    for (usize i = 0; i < state.slots.size(); ++i) {
        const auto& saved = state.slots[i];
        if (saved.index != m_catalog->slots[i]) {
            return std::nullopt;
        }
        auto& local = m_scene.m_slots.at(saved.index);
        if (saved.texture != m_catalog->id(local.texture) || saved.usable != local.usable ||
            saved.thermal != local.thermal) {
            return std::nullopt;
        }
        Prepared::Data::Slot slot{
            &local, saved.frame ? m_catalog->pointers[*saved.frame] : nullptr, {}};
        for (const auto id : saved.cycle) {
            slot.cycle.push_back(m_catalog->pointers[id]);
        }
        data->slots.push_back(std::move(slot));
    }
    for (usize i = 0; i < state.moving.size(); ++i) {
        const auto& saved = state.moving[i];
        const auto& local = m_collision.m_moving[i];
        if (saved.object != local.object || !sameTriangles(local.local, saved.local)) {
            return std::nullopt;
        }
        auto staged = local;
        staged.place(saved.transform);
        if (!sameTriangles(staged.placed, saved.placed) || staged.boundsMin != saved.minimum ||
            staged.boundsMax != saved.maximum) {
            return std::nullopt;
        }
        data->moving.push_back(std::move(staged));
    }
    for (const auto& event : state.cycles) {
        data->events.push_back({event.object, event.wrapped});
    }
    Prepared prepared{std::move(data)};
    return canCommit(prepared) ? std::optional{std::move(prepared)} : std::nullopt;
}

bool WorldStateCheckpoint::canCommit(const Prepared& prepared) const {
    if (!prepared.m_data || !ready()) {
        return false;
    }
    const auto& data = *prepared.m_data;
    return data.wrapper == this && data.lifetime == m_lifetime && data.catalog == m_catalog &&
           data.generation == m_generation && m_generation != std::numeric_limits<u64>::max();
}
bool WorldStateCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    const auto& state = data->state;
    for (usize i = 0; i < state.placements.size(); ++i) {
        auto& local = m_scene.m_placements[i];
        const auto& saved = state.placements[i];
        local.local = saved.local;
        local.previous = saved.previous;
        local.continuity = saved.continuity;
    }
    for (usize i = 0; i < state.units.size(); ++i) {
        m_scene.m_units[i].alpha = state.units[i].alpha;
        m_scene.m_units[i].visible = state.units[i].visible;
    }
    for (usize i = 0; i < data->slots.size(); ++i) {
        auto& local = *data->slots[i].target;
        const auto& saved = state.slots[i];
        local.frame = data->slots[i].frame;
        local.cycle.swap(data->slots[i].cycle);
        local.cyclePosition = saved.cyclePosition;
        local.cycleSpeed = saved.cycleSpeed;
        local.offset = saved.offset;
        local.scrollPhase = saved.scrollPhase;
        local.scrollVelocity = saved.scrollVelocity;
    }
    std::swap(m_scene.m_lighting, data->lighting);
    m_scene.m_darken = state.darken;
    m_scene.m_textureFrame = state.textureFrame;
    std::fill(m_scene.m_worldValid.begin(), m_scene.m_worldValid.end(), u8{0});
    std::fill(m_scene.m_presentedValid.begin(), m_scene.m_presentedValid.end(), u8{0});
    for (usize i = 0; i < data->moving.size(); ++i) {
        auto& local = m_collision.m_moving[i];
        auto& saved = data->moving[i];
        local.world = saved.world;
        local.boundsMin = saved.boundsMin;
        local.boundsMax = saved.boundsMax;
        local.placed.swap(saved.placed);
    }
    m_collision.m_hidden.swap(data->state.hidden);
    m_collision.m_contactOnly.swap(data->state.contactOnly);
    m_collision.m_blockedFloorExits.swap(data->state.blockedFloorExits);
    for (usize i = 0; i < state.animations.size(); ++i) {
        auto& track = m_animator.m_tracks[i];
        const auto& saved = state.animations[i];
        track.frame = saved.frame;
        track.reverse = saved.reverse;
        track.once = saved.once;
        track.finished = saved.finished;
        track.held = saved.held;
        track.presentationCut = saved.presentationCut;
    }
    m_animator.m_cycleEvents.swap(data->events);
    ++m_generation;
    return true;
}
bool WorldStateCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl
