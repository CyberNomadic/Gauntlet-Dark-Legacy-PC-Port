#include "engine/world/WorldStateCheckpoint.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>

#include "engine/io/Sha256.h"

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
bool validTriangles(std::span<const CollisionTriangle> values, usize& count) {
    if (values.size() > WorldStateCheckpoint::State::kMaxTriangles - count) {
        return false;
    }
    count += values.size();
    return std::ranges::all_of(values, [](const auto& value) {
        return value.object >= -1 && value.object <= 1'000'000 && vector(value.normal) &&
               std::ranges::all_of(value.vertices, vector);
    });
}
s32 textureOrigin(s32 counter, bool keyed, s32 period, s32 rate, u32 frame) {
    s64 start = counter;
    if (!keyed && period > 0) {
        start -= frame / static_cast<u32>(std::max(rate, 1));
        start %= period;
        if (start < 0) {
            start += period;
        }
    }
    return static_cast<s32>(start);
}

// No object representations, padding, addresses, unordered iteration or native
// endianness enter the catalog. This work runs only when trusted assets bind.
class Fingerprint {
public:
    void add(u64 value) {
        std::array<u8, 8> bytes{};
        for (u32 i = 0; i < 8; ++i) {
            bytes[i] = static_cast<u8>(value >> (8 * i));
        }
        m_hash.update(bytes);
    }
    void add(u32 value) { add(static_cast<u64>(value)); }
    void add(s32 value) { add(std::bit_cast<u32>(value)); }
    void add(u16 value) { add(static_cast<u32>(value)); }
    void add(u8 value) { add(static_cast<u32>(value)); }
    void add(bool value) { add(value ? 1U : 0U); }
    void add(f32 value) { add(std::bit_cast<u32>(value)); }
    void add(const Vec2& value) {
        add(value.x);
        add(value.y);
    }
    void add(const Vec3& value) {
        add(value.x);
        add(value.y);
        add(value.z);
    }
    void add(const Color& value) {
        add(value.r);
        add(value.g);
        add(value.b);
        add(value.a);
    }
    void add(const Mat4& value) {
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 4; ++row) {
                add(value[column][row]);
            }
        }
    }
    void add(std::string_view value) {
        add(static_cast<u64>(value.size()));
        for (const char byte : value) {
            add(static_cast<u8>(byte));
        }
    }
    template <typename Range> void sequence(const Range& values) {
        add(static_cast<u64>(values.size()));
        for (const auto& value : values) {
            add(value);
        }
    }
    void add(const CollisionTriangle& value) {
        add(value.normal);
        sequence(value.vertices);
        add(value.object);
        add(value.objectFlags);
    }
    void add(const TrackInfo& value) {
        add(value.node);
        add(value.flags);
        sequence(value.frames);
        sequence(value.values);
    }
    void add(const TextureAnimationInfo& value) {
        add(value.name);
        add(value.frameName);
        add(value.texture);
        add(value.source);
        add(value.frames);
        add(value.start);
        add(value.rate);
        add(value.offset);
        add(value.flag);
    }
    void add(const MeshPart& value) {
        add(value.texture);
        add(value.lightmap);
        sequence(value.indices);
    }
    void add(const MeshVertex& value) {
        add(value.position);
        add(value.normal);
        add(value.uv);
        add(value.lightmapUv);
        add(value.color);
    }
    void add(const ImmediateVertex& value) {
        add(value.position);
        add(value.color);
        add(value.uv);
        add(value.uv2);
    }
    void add(const Mesh& value) {
        sequence(value.vertices);
        sequence(value.parts);
        add(value.prelit);
        add(value.flags);
    }
    void add(const WorldLayout& layout) {
        add(layout.minBounds());
        add(layout.maxBounds());
        add(static_cast<u64>(layout.objects().size()));
        for (const auto& object : layout.objects()) {
            add(object.name);
            add(object.position);
            add(object.flags);
            add(object.objectFlags);
            add(object.next);
            add(object.child);
            add(object.parent);
            add(object.radius);
            add(object.noCollision);
        }
        add(static_cast<u64>(layout.animations().size()));
        for (const auto& animation : layout.animations()) {
            add(animation.object);
            add(animation.frames);
            add(animation.state);
            add(animation.start);
            add(animation.track);
        }
        add(static_cast<u64>(layout.locators().size()));
        for (const auto& locator : layout.locators()) {
            add(static_cast<u8>(locator.kind));
            add(locator.delay);
            add(locator.next);
            add(locator.position);
            add(locator.rotation);
        }
        add(static_cast<u64>(layout.itemInfos().size()));
        for (const auto& info : layout.itemInfos()) {
            add(info.type);
            add(info.subtype);
            add(info.name);
            add(info.radius);
            add(info.height);
            add(info.xSize);
            add(info.zSize);
            add(info.collisionType);
            add(info.collisionFlags);
            add(info.collisionOffset);
            add(info.objectFlags);
            add(info.properties);
            add(info.value);
            add(info.armor);
            add(info.hitPoints);
            add(info.activeType);
            add(info.activeOff);
            add(info.activeOn);
            sequence(info.choices);
        }
        add(static_cast<u64>(layout.itemInstances().size()));
        for (const auto& item : layout.itemInstances()) {
            add(item.info);
            add(item.minPlayers);
            add(item.flags);
            add(item.name);
            add(item.position);
            add(item.rotation);
            sequence(item.params);
            add(static_cast<u64>(item.collision.size()));
            for (const auto& triangle : item.collision) {
                add(triangle.normal);
                sequence(triangle.vertices);
            }
        }
        add(static_cast<u64>(layout.particleTemplates().size()));
        for (const auto& particle : layout.particleTemplates()) {
            add(static_cast<u8>(particle.id));
            add(particle.preset);
            add(particle.flags);
            add(particle.flagMask);
            add(particle.enables);
            add(particle.maxParticles);
            add(particle.maxDirections);
            add(particle.maxPositions);
            sequence(particle.emitterLife);
            sequence(particle.particleLife);
            add(particle.angle);
            add(particle.textureCount);
            add(particle.texture);
            add(particle.direction);
            add(particle.volume);
            sequence(particle.rate);
            add(particle.rateRandom);
            add(particle.gravity);
            add(particle.drag);
            add(particle.speed);
            sequence(particle.rgba);
            sequence(particle.width);
            add(particle.delay);
        }
    }
    std::array<u8, 32> finish() const { return m_hash.finish(); }

private:
    Sha256 m_hash;
};
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
    if (resourceCount > kMaxTextures || slots.size() > kMaxTextures ||
        placements.size() > kMaxObjects || units.size() > placements.size() ||
        moving.size() > kMaxObjects || animations.size() > kMaxAnimations ||
        cycles.size() > kMaxEvents || !light.valid() || !scalar(darken) || !mask(hidden, -1) ||
        !mask(contactOnly, -1) || !mask(blockedFloorExits, 0)) {
        return false;
    }
    for (const auto& placement : placements) {
        if (!matrix(placement.local) || !matrix(placement.previous)) {
            return false;
        }
    }
    std::set<u32> unitObjects;
    for (usize i = 0; i < units.size(); ++i) {
        const auto& unit = units[i];
        if (unit.object >= placements.size() || !unitObjects.insert(unit.object).second ||
            !scalar(unit.alpha) || unit.alpha < 0 || unit.alpha > 1) {
            return false;
        }
    }
    usize cyclesSize = 0;
    u32 previous = 0;
    bool first = true;
    const auto resourceValid = [&](const std::optional<u32>& id) {
        return !id || *id < resourceCount;
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
            if (id >= resourceCount) {
                return false;
            }
        }
        previous = slot.index;
        first = false;
    }
    std::set<s32> movers;
    for (const auto& object : moving) {
        if (object.object < 0 || object.object > 1'000'000 ||
            !movers.insert(object.object).second || !matrix(object.transform)) {
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
    std::shared_ptr<const std::vector<TextureAnimationInfo>> textureAuthored;
    const WorldLayout* authored = nullptr;
    const WorldObject* objects = nullptr;
    std::array<u8, 32> fingerprint{};
    std::vector<s32> collisionObjects;
    std::vector<TextureAnimator::Entry> textureEntries;
    std::vector<s32> textureOrigins;
    std::vector<s32> entryOfInfo;
    const TextureAnimator::Entry* textureStorage = nullptr;
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

bool WorldStateCheckpoint::bind(const TextureAnimator& textures, const WorldLayout& layout) {
    clear();
    if (!m_binding || m_scene.m_slots.size() > State::kMaxTextures ||
        textures.size() > State::kMaxTextures || m_scene.m_placements.size() > State::kMaxObjects ||
        m_animator.size() > State::kMaxAnimations ||
        layout.objects().size() != m_scene.m_placements.size() ||
        m_collision.m_moving.size() > State::kMaxObjects) {
        return false;
    }
    auto catalog = std::make_shared<Catalog>();
    catalog->binding = m_binding;
    catalog->textures = &textures;
    catalog->textureAuthored = textures.m_authored;
    catalog->authored = &layout;
    catalog->objects = layout.objects().data();
    catalog->textureEntries = textures.m_entries;
    catalog->entryOfInfo = textures.m_entryOfInfo;
    catalog->textureStorage = textures.m_entries.data();
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
    Fingerprint hash;
    hash.add(std::string_view{"GDL bound world catalog 1"});
    hash.add(layout);
    hash.add(catalog->layout);
    hash.add(static_cast<u64>(m_scene.m_placements.size()));
    for (const auto& placement : m_scene.m_placements) {
        hash.add(placement.initial);
        hash.add(placement.parent);
        hash.add(placement.unit);
        hash.add(placement.moving);
    }
    hash.add(static_cast<u64>(m_scene.m_units.size()));
    for (const auto& unit : m_scene.m_units) {
        hash.add(static_cast<u64>(unit.object));
        hash.add(unit.sortBias);
        hash.add(unit.facing);
        hash.add(unit.prelit);
        hash.add(unit.chrome);
        hash.add(unit.sorted);
        hash.add(unit.deferAlways);
        hash.add(unit.meshAlpha);
        hash.add(unit.background);
        hash.add(unit.depthWrite);
        hash.add(unit.depthTest);
        hash.add(unit.mesh != nullptr);
        if (unit.mesh) {
            hash.add(*unit.mesh);
        }
        hash.add(static_cast<u64>(unit.parts.size()));
        for (const auto& part : unit.parts) {
            hash.add(part.slot);
            hash.add(part.lightmapScale);
            hash.add(part.additive);
            hash.add(part.part != nullptr);
            if (part.part) {
                hash.add(*part.part);
            }
            hash.add(part.lightmap != nullptr);
            if (part.lightmap) {
                hash.add(part.lightmap->width());
                hash.add(part.lightmap->height());
            }
        }
    }
    hash.add(static_cast<u64>(m_scene.m_batches.size()));
    for (const auto& batch : m_scene.m_batches) {
        hash.add(batch.slot);
        hash.add(batch.lightmapScale);
        hash.add(batch.lowest);
        hash.add(batch.highest);
        hash.add(batch.additive);
        hash.add(batch.depthWrite);
        hash.add(batch.depthTest);
        hash.sequence(batch.geometry.triangles());
        hash.sequence(batch.normals);
        hash.add(batch.lightmap != nullptr);
        if (batch.lightmap) {
            hash.add(batch.lightmap->width());
            hash.add(batch.lightmap->height());
        }
    }
    usize triangleCount = 0;
    if (!validTriangles(m_collision.m_triangles, triangleCount)) {
        return false;
    }
    hash.sequence(m_collision.m_triangles);
    for (const auto& triangle : m_collision.m_triangles) {
        catalog->collisionObjects.push_back(triangle.object);
    }
    hash.add(static_cast<u64>(m_collision.m_moving.size()));
    for (const auto& moving : m_collision.m_moving) {
        if (moving.local.empty() || !validTriangles(moving.local, triangleCount)) {
            return false;
        }
        catalog->collisionObjects.push_back(moving.object);
        hash.add(moving.object);
        hash.sequence(moving.local);
    }
    std::ranges::sort(catalog->collisionObjects);
    const auto repeated = std::ranges::unique(catalog->collisionObjects);
    catalog->collisionObjects.erase(repeated.begin(), repeated.end());
    hash.add(m_collision.m_min);
    hash.add(m_collision.m_max);
    hash.add(m_collision.m_columns);
    hash.add(m_collision.m_rows);
    hash.add(static_cast<u64>(m_collision.m_cells.size()));
    for (const auto& cell : m_collision.m_cells) {
        hash.sequence(cell);
    }
    hash.add(static_cast<u64>(m_animator.m_tracks.size()));
    for (const auto& track : m_animator.m_tracks) {
        hash.add(track.object);
        hash.add(track.frames);
        hash.add(track.origin);
        hash.add(track.track);
    }
    hash.add(static_cast<u64>(catalog->resources.size()));
    for (usize i = 0; i < catalog->resources.size(); ++i) {
        const auto& resource = catalog->resources[i];
        hash.add(static_cast<u8>(resource.kind));
        hash.add(resource.index);
        hash.add(resource.frame);
        hash.add(catalog->pointers[i]->width());
        hash.add(catalog->pointers[i]->height());
    }
    hash.add(static_cast<u64>(catalog->slots.size()));
    for (const auto index : catalog->slots) {
        const auto& slot = m_scene.m_slots.at(index);
        hash.add(index);
        hash.add(catalog->id(slot.texture).value_or(static_cast<u32>(State::kMaxTextures)));
        hash.add(slot.usable);
        hash.add(slot.thermal);
    }
    hash.add(static_cast<bool>(textures.m_authored));
    if (textures.m_authored) {
        hash.sequence(*textures.m_authored);
    }
    hash.add(static_cast<u64>(textures.m_entries.size()));
    for (const auto& entry : textures.m_entries) {
        hash.add(entry.slot);
        hash.add(entry.direction);
        hash.add(entry.period);
        hash.add(entry.rate);
        hash.add(entry.keyed);
        hash.add(entry.offset);
        hash.add(entry.fade);
        // Recover the authored start from the live clock so a late catalog bind
        // does not accidentally fingerprint the current animation phase.
        const s32 start =
            textureOrigin(entry.counter, entry.keyed, entry.period, entry.rate, textures.m_frame);
        catalog->textureOrigins.push_back(start);
        hash.add(start);
        hash.add(static_cast<u64>(entry.frames.size()));
        for (const auto* frame : entry.frames) {
            const auto id = catalog->id(frame);
            if (!id) {
                return false;
            }
            hash.add(*id);
        }
    }
    hash.sequence(textures.m_entryOfInfo);
    catalog->fingerprint = hash.finish();
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
        m_catalog->slots.size() != m_scene.m_slots.size() ||
        m_catalog->objects != m_catalog->authored->objects().data() ||
        m_catalog->authored->objects().size() != m_catalog->placementCount ||
        m_catalog->textureStorage != m_catalog->textures->m_entries.data() ||
        m_catalog->textureAuthored != m_catalog->textures->m_authored ||
        m_catalog->textureEntries.size() != m_catalog->textures->m_entries.size() ||
        m_catalog->entryOfInfo != m_catalog->textures->m_entryOfInfo) {
        return false;
    }
    for (usize i = 0; i < m_catalog->textureEntries.size(); ++i) {
        const auto& expected = m_catalog->textureEntries[i];
        const auto& current = m_catalog->textures->m_entries[i];
        if (expected.slot != current.slot || expected.frames != current.frames ||
            expected.direction != current.direction || expected.period != current.period ||
            expected.rate != current.rate || expected.keyed != current.keyed ||
            expected.offset != current.offset || expected.fade != current.fade ||
            m_catalog->textureOrigins[i] != textureOrigin(current.counter, current.keyed,
                                                          current.period, current.rate,
                                                          m_catalog->textures->m_frame)) {
            return false;
        }
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

bool WorldStateCheckpoint::hasCollisionObject(s32 object) const {
    return ready() && std::ranges::binary_search(m_catalog->collisionObjects, object);
}
std::span<const s32> WorldStateCheckpoint::collisionObjects() const {
    return ready() ? std::span<const s32>{m_catalog->collisionObjects} : std::span<const s32>{};
}

std::optional<WorldStateCheckpoint::State> WorldStateCheckpoint::checkpoint() const {
    if (!ready()) {
        return std::nullopt;
    }
    State state;
    state.layout = m_scene.m_layoutSignature;
    state.catalog = m_catalog->fingerprint;
    state.resourceCount = static_cast<u32>(m_catalog->resources.size());
    for (const auto& placement : m_scene.m_placements) {
        state.placements.push_back({placement.local, placement.previous, placement.continuity});
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
    usize triangleCount = 0;
    for (const auto& object : m_collision.m_moving) {
        if (!vector(object.boundsMin) || !vector(object.boundsMax) ||
            !validTriangles(object.placed, triangleCount)) {
            return std::nullopt;
        }
        state.moving.push_back({object.object, object.world});
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
        state.catalog != m_catalog->fingerprint ||
        state.resourceCount != m_catalog->resources.size() ||
        state.placements.size() != m_scene.m_placements.size() ||
        state.units.size() != m_scene.m_units.size() ||
        state.slots.size() != m_catalog->slots.size() ||
        state.moving.size() != m_collision.m_moving.size() ||
        state.animations.size() != m_animator.size() ||
        m_generation == std::numeric_limits<u64>::max()) {
        return std::nullopt;
    }
    const auto knownMask = [&](s32 object) {
        return object == -1 ||
               (object >= 0 && static_cast<usize>(object) < state.placements.size()) ||
               std::ranges::binary_search(m_catalog->collisionObjects, object);
    };
    if (!std::ranges::all_of(state.hidden, knownMask) ||
        !std::ranges::all_of(state.contactOnly, knownMask) ||
        !std::ranges::all_of(state.blockedFloorExits, knownMask)) {
        return std::nullopt;
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
        if (saved.object != local.object) {
            return std::nullopt;
        }
        auto staged = local;
        staged.place(saved.transform);
        usize triangleCount = 0;
        if (!validTriangles(staged.placed, triangleCount) || !vector(staged.boundsMin) ||
            !vector(staged.boundsMax)) {
            return std::nullopt;
        }
        // The preparation owns only newly derived caches; immutable triangle
        // inputs remain with the already-bound collision owner.
        std::vector<CollisionTriangle>{}.swap(staged.local);
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
