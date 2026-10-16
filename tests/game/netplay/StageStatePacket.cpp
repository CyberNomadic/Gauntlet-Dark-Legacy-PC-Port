#include "game/netplay/StageStatePacket.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <type_traits>
#include <utility>

#include <lz4.h>

#include "engine/codec/BytePlanes.h"
#include "engine/io/ByteReader.h"
#include "engine/io/Sha256.h"

#include "game/netplay/AnimationCheckpoint.h"
#include "game/netplay/CanonicalDigestSink.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("STST");
constexpr usize kCatalogWalkBytes = 16 * 1024 * 1024;
constexpr u32 kRaw = 0, kLz4 = 1, kLz4BytePlanes = 2;
constexpr usize kCompressionMinimum = 1024;
static_assert(StageStatePacket::kMaxDecodedBytes <
              static_cast<usize>(std::numeric_limits<s32>::max()));
std::vector<u8> compress(std::span<const u8> bytes) {
    const auto size = static_cast<s32>(bytes.size());
    // The destination is itself wire-bounded: an incompressible stage cannot
    // allocate an arbitrarily large encoded candidate.
    const auto capacity =
        static_cast<s32>(std::min(static_cast<usize>(LZ4_compressBound(size)),
                                  StageStatePacket::kMaxBytes - StageStatePacket::kHeaderBytes));
    std::vector<u8> compressed(static_cast<usize>(capacity));
    // LZ4 char pointers describe byte buffers, not reinterpreted objects.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    const s32 written =
        LZ4_compress_default(reinterpret_cast<const char*>(bytes.data()),
                             reinterpret_cast<char*>(compressed.data()), size, capacity);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    compressed.resize(written > 0 && written < size ? static_cast<usize>(written) : 0);
    return compressed;
}
struct GeometryCatalog {
    std::array<u8, 32> fingerprint{}, core{}, transporters{};
    LevelRef level;
    std::vector<std::vector<ItemCollisionTriangle>> items;
    std::vector<std::vector<CollisionTriangle>> walls;
    std::map<std::string, u32> itemIds, wallIds;
};

class Writer {
public:
    static constexpr bool reading = false;
    explicit Writer(const GeometryCatalog* geometry = nullptr,
                    GeometryCatalog* collecting = nullptr)
        : catalog(geometry), collect(collecting) {}
    std::vector<u8> bytes;
    CanonicalDigestSink* digest = nullptr;
    bool agreement = false;
    usize records = 0, strings = 0, triangles = 0, used = 0;
    usize limit = StageStatePacket::kMaxBytes;
    bool countOnly = false;
    const GeometryCatalog* catalog = nullptr;
    GeometryCatalog* collect = nullptr;
    void raw(std::span<const u8> source) {
        if (source.size() > remaining()) {
            throw FormatError("stage byte budget");
        }
        used += source.size();
        if (digest) {
            digest->append(source);
        } else if (!collect && !countOnly) {
            bytes.insert(bytes.end(), source.begin(), source.end());
        }
    }
    void word(u32 value) {
        std::array<u8, 4> data{};
        for (usize index = 0; index < data.size(); ++index) {
            data[index] = static_cast<u8>(value >> (index * 8));
        }
        raw(data);
    }
    void text(const std::string& value) {
        raw({reinterpret_cast<const u8*>(value.data()), value.size()});
    }
    template <class T> void number(T value) {
        if constexpr (std::is_enum_v<T>) {
            number(static_cast<std::underlying_type_t<T>>(value));
        } else if constexpr (std::is_same_v<T, bool>) {
            word(value ? 1U : 0U);
        } else if constexpr (std::is_floating_point_v<T>) {
            if (!std::isfinite(value)) {
                throw FormatError("nonfinite stage scalar");
            }
            if constexpr (sizeof(T) == 4) {
                word(std::bit_cast<u32>(value));
            } else {
                number(std::bit_cast<u64>(value));
            }
        } else if constexpr (sizeof(T) == 8) {
            const auto bits = static_cast<u64>(value);
            word(static_cast<u32>(bits));
            word(static_cast<u32>(bits >> 32));
        } else if constexpr (std::is_signed_v<T>) {
            word(std::bit_cast<u32>(static_cast<s32>(value)));
        } else {
            word(static_cast<u32>(value));
        }
    }
    usize remaining() const { return (collect ? kCatalogWalkBytes : limit) - used; }
};
class Reader {
public:
    static constexpr bool reading = true;
    Reader(std::span<const u8> bytes, const GeometryCatalog* geometry = nullptr)
        : catalog(geometry), m_reader(bytes) {}
    usize records = 0, strings = 0, triangles = 0;
    const GeometryCatalog* catalog = nullptr;
    std::span<const u8> raw(usize count) { return m_reader.readBytes(count); }
    template <class T> void number(T& value) {
        if constexpr (std::is_enum_v<T>) {
            std::underlying_type_t<T> decoded{};
            number(decoded);
            value = static_cast<T>(decoded);
        } else if constexpr (std::is_same_v<T, bool>) {
            const u32 bits = m_reader.readU32();
            if (bits > 1) {
                throw FormatError("noncanonical stage boolean");
            }
            value = bits != 0;
        } else if constexpr (std::is_floating_point_v<T>) {
            if constexpr (sizeof(T) == 4) {
                value = std::bit_cast<f32>(m_reader.readU32());
            } else {
                u64 bits = 0;
                number(bits);
                value = std::bit_cast<f64>(bits);
            }
            if (!std::isfinite(value)) {
                throw FormatError("nonfinite stage scalar");
            }
        } else if constexpr (sizeof(T) == 8) {
            const u64 low = m_reader.readU32(), high = m_reader.readU32();
            value = std::bit_cast<T>(low | (high << 32));
        } else if constexpr (std::is_signed_v<T>) {
            const s32 decoded = std::bit_cast<s32>(m_reader.readU32());
            if (decoded < std::numeric_limits<T>::min() ||
                decoded > std::numeric_limits<T>::max()) {
                throw FormatError("stage signed overflow");
            }
            value = static_cast<T>(decoded);
        } else {
            const u32 decoded = m_reader.readU32();
            if (decoded > std::numeric_limits<T>::max()) {
                throw FormatError("stage unsigned overflow");
            }
            value = static_cast<T>(decoded);
        }
    }
    usize remaining() const { return m_reader.remaining(); }

private:
    ByteReader m_reader;
};
template <class T> struct IsVector : std::false_type {};
template <class T, class A> struct IsVector<std::vector<T, A>> : std::true_type {};
template <class T> struct IsOptional : std::false_type {};
template <class T> struct IsOptional<std::optional<T>> : std::true_type {};
template <class T> struct IsArray : std::false_type {};
template <class T, usize N> struct IsArray<std::array<T, N>> : std::true_type {};
template <class T> struct IsPair : std::false_type {};
template <class T, class U> struct IsPair<std::pair<T, U>> : std::true_type {};
template <class A, class T> void field(A&, T&);
/** These optional poses are overwritten by draw before use. The agreement
 * visitor alone elides them; wire and native restore retain and validate them. */
template <class A, class T> void drawCache(A& a, T& value) {
    if constexpr (!A::reading) {
        if (a.agreement) {
            const std::remove_cv_t<T> empty{};
            field(a, empty);
            return;
        }
    }
    field(a, value);
}

template <class A, class... T> void fields(A& a, T&... v) {
    (field(a, v), ...);
}
template <class A, class T> void index(A& a, T& value) {
    u64 bits = value;
    a.number(bits);
    if constexpr (A::reading) {
        if (bits > std::numeric_limits<usize>::max()) {
            throw FormatError("stage index overflow");
        }
        value = static_cast<usize>(bits);
    }
}
template <class A, class T, class F> void sequence(A& a, T& value, F entry) {
    u32 count = 0;
    if constexpr (!A::reading) {
        if (value.size() > StageStatePacket::kMaxRecords) {
            throw FormatError("stage index count");
        }
        count = static_cast<u32>(value.size());
    }
    a.number(count);
    if (count > StageStatePacket::kMaxRecords - a.records || count > a.remaining() / 8) {
        throw FormatError("stage index budget");
    }
    a.records += count;
    if constexpr (A::reading) {
        value.resize(count);
    }
    for (auto& item : value) {
        entry(a, item);
    }
}
template <class A, class T> void indices(A& a, T& value) {
    sequence(a, value, [](auto& archive, auto& item) { index(archive, item); });
}
template <class A, class T> void optionalIndex(A& a, T& value) {
    bool present = value.has_value();
    a.number(present);
    if constexpr (A::reading) {
        if (present) {
            value.emplace();
        } else {
            value.reset();
        }
    }
    if (present) {
        index(a, *value);
    }
}
template <class A, class T> void indexTimers(A& a, T& value) {
    sequence(a, value, [](auto& archive, auto& item) {
        index(archive, item.first);
        field(archive, item.second);
    });
}

template <class T> std::string geometryKey(const std::vector<T>& triangles) {
    Writer writer;
    writer.limit = kCatalogWalkBytes;
    // Geometry is never transported; this separate local work cap bounds the
    // canonical key and all catalog-backed allocations before a copy is made.
    if (triangles.size() > StageStatePacket::kMaxCatalogTriangles) {
        throw FormatError("stage geometry catalog entry limit");
    }
    writer.number(static_cast<u32>(triangles.size()));
    for (const auto& triangle : triangles) {
        field(writer, triangle);
    }
    return {writer.bytes.begin(), writer.bytes.end()};
}
template <class T> const auto& geometryRows(const GeometryCatalog& catalog) {
    if constexpr (std::is_same_v<T, ItemCollisionTriangle>) {
        return catalog.items;
    } else {
        return catalog.walls;
    }
}
template <class T> auto& geometryRows(GeometryCatalog& catalog) {
    if constexpr (std::is_same_v<T, ItemCollisionTriangle>) {
        return catalog.items;
    } else {
        return catalog.walls;
    }
}
template <class T> const auto& geometryIds(const GeometryCatalog& catalog) {
    if constexpr (std::is_same_v<T, ItemCollisionTriangle>) {
        return catalog.itemIds;
    } else {
        return catalog.wallIds;
    }
}
template <class T> auto& geometryIds(GeometryCatalog& catalog) {
    if constexpr (std::is_same_v<T, ItemCollisionTriangle>) {
        return catalog.itemIds;
    } else {
        return catalog.wallIds;
    }
}
template <class A, class T> void geometry(A& a, T& value) {
    using Triangle = typename std::remove_cv_t<T>::value_type;
    u32 id = 0;
    if constexpr (A::reading) {
        a.number(id);
        const auto& rows = geometryRows<Triangle>(*a.catalog);
        if (id >= rows.size() ||
            rows[id].size() > StageStatePacket::kMaxCatalogTriangles - a.triangles) {
            throw FormatError("stage geometry reference or copy budget");
        }
        a.triangles += rows[id].size();
        value = rows[id];
    } else {
        if (value.size() > StageStatePacket::kMaxCatalogTriangles - a.triangles) {
            throw FormatError("stage geometry aggregate budget");
        }
        a.triangles += value.size();
        const auto key = geometryKey(value);
        if (a.collect) {
            auto& ids = geometryIds<Triangle>(*a.collect);
            auto& rows = geometryRows<Triangle>(*a.collect);
            const auto [found, inserted] = ids.emplace(key, static_cast<u32>(rows.size()));
            if (inserted) {
                rows.push_back(value);
            }
            id = found->second;
        } else {
            const auto& ids = geometryIds<Triangle>(*a.catalog);
            const auto found = ids.find(key);
            if (found == ids.end()) {
                throw FormatError("stage geometry not in trusted catalog");
            }
            id = found->second;
        }
        a.number(id);
    }
}
template <class T> constexpr usize recordLimit() {
    if constexpr (std::is_same_v<T, Particle>) {
        return ParticleDescriptor::kMostParticles;
    } else if constexpr (std::is_same_v<T, TowerCameraCheckpoint::Marker>) {
        return TowerCameraCheckpoint::State::kMaxMarkers;
    } else if constexpr (std::is_same_v<T, ExitPortalsCheckpoint::Portal>) {
        return ExitPortalsCheckpoint::State::kMaxPortals;
    } else if constexpr (std::is_same_v<T, LevelTransportersCheckpoint::Pad>) {
        return LevelTransportersCheckpoint::State::kMaxPads;
    } else if constexpr (std::is_same_v<T, ExitPortalsCheckpoint::Visitor>) {
        return 4;
    } else if constexpr (std::is_same_v<T, TreePose::Checkpoint::Node> ||
                         std::is_same_v<T, NodePose>) {
        return TreePose::Checkpoint::kMaxNodes;
    } else {
        return StageStatePacket::kMaxRecords;
    }
}
template <class T> constexpr usize minimumBytes() {
    if constexpr (std::is_same_v<T, TowerCameraCheckpoint::Marker>) {
        return 32;
    } else if constexpr (std::is_same_v<T, WorldStateCheckpoint::Placement>) {
        return 12;
    } else if constexpr (std::is_same_v<T, WorldStateCheckpoint::Moving> ||
                         std::is_same_v<T, RubbleCheckpoint::Piece>) {
        return 8;
    } else if constexpr (std::is_same_v<T, ChestsCheckpoint::Chest> ||
                         std::is_same_v<T, LockedGatesCheckpoint::Gate> ||
                         std::is_same_v<T, TrapsCheckpoint::Trap> ||
                         std::is_same_v<T, BreakablesCheckpoint::Barrel> ||
                         std::is_same_v<T, StaticSceneryCheckpoint::Prop> ||
                         std::is_same_v<T, ExitPortalsCheckpoint::Portal>) {
        return 256;
    } else if constexpr (std::is_same_v<T, PlacedItemsCheckpoint::Item>) {
        return 220;
    } else if constexpr (std::is_same_v<T, ParticleField::Checkpoint::Entry> ||
                         std::is_same_v<T, DestructibleWallsCheckpoint::Wall> ||
                         std::is_same_v<T, FallingSceneryCheckpoint::Piece> ||
                         std::is_same_v<T, RotatorsCheckpoint::Rotator> ||
                         std::is_same_v<T, LevelTransportersCheckpoint::Pad> ||
                         std::is_same_v<T, SafeRocksCheckpoint::Rock>) {
        return 128;
    } else if constexpr (std::is_same_v<T, Particle>) {
        return 28;
    } else if constexpr (std::is_same_v<T, Vec3> || std::is_same_v<T, WorldStateCheckpoint::Unit>) {
        return 12;
    } else {
        return 4;
    }
}

// One explicit field list drives both directions. Scalars have fixed widths;
// native usize values always occupy 64 bits (even on a 32-bit receiver).
template <class A, class T> void field(A& a, T& v) {
    using V = std::remove_cv_t<T>;
    if constexpr (std::is_arithmetic_v<V> || std::is_enum_v<V>) {
        a.number(v);
    } else if constexpr (std::is_same_v<V, Vec2>) {
        fields(a, v.x, v.y);
    } else if constexpr (std::is_same_v<V, Vec3>) {
        fields(a, v.x, v.y, v.z);
    } else if constexpr (std::is_same_v<V, Mat4>) {
        // Column-major IEEE-754 bits relative only to exact identity, never a
        // phase-dependent catalog pose. Signed zero must remain distinguishable.
        u32 mask = 0;
        if constexpr (!A::reading) {
            for (s32 column = 0; column < 4; ++column) {
                for (s32 row = 0; row < 4; ++row) {
                    const f32 identity = column == row ? 1.0f : 0.0f;
                    if (std::bit_cast<u32>(v[column][row]) != std::bit_cast<u32>(identity)) {
                        mask |= 1U << static_cast<u32>(column * 4 + row);
                    }
                }
            }
        }
        a.number(mask);
        if ((mask & 0xffff0000U) != 0) {
            throw FormatError("stage matrix reserved mask");
        }
        if constexpr (A::reading) {
            v = Mat4{1};
        }
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 4; ++row) {
                if ((mask & (1U << static_cast<u32>(column * 4 + row))) == 0) {
                    continue;
                }
                a.number(v[column][row]);
                if constexpr (A::reading) {
                    const f32 identity = column == row ? 1.0f : 0.0f;
                    if (std::bit_cast<u32>(v[column][row]) == std::bit_cast<u32>(identity)) {
                        throw FormatError("redundant stage matrix element");
                    }
                }
            }
        }
    } else if constexpr (std::is_same_v<V, std::string>) {
        u32 count = 0;
        if constexpr (!A::reading) {
            if (v.size() > StageStatePacket::kMaxString) {
                throw FormatError("stage string limit");
            }
            count = static_cast<u32>(v.size());
        }
        a.number(count);
        if (count > StageStatePacket::kMaxString || count > a.remaining() ||
            count > StageStatePacket::kMaxStringBytes - a.strings) {
            throw FormatError("stage string budget");
        }
        a.strings += count;
        if constexpr (A::reading) {
            const auto bytes = a.raw(count);
            v.assign(bytes.begin(), bytes.end());
        } else {
            a.text(v);
        }
    } else if constexpr (std::is_same_v<V, std::vector<ItemCollisionTriangle>> ||
                         std::is_same_v<V, std::vector<CollisionTriangle>>) {
        geometry(a, v);
    } else if constexpr (IsVector<V>::value) {
        u32 count = 0;
        if constexpr (!A::reading) {
            if (v.size() > recordLimit<typename V::value_type>()) {
                throw FormatError("stage record limit");
            }
            count = static_cast<u32>(v.size());
        }
        a.number(count);
        if (count > recordLimit<typename V::value_type>() ||
            count > StageStatePacket::kMaxRecords - a.records ||
            count > a.remaining() / minimumBytes<typename V::value_type>()) {
            throw FormatError("stage aggregate record budget");
        }
        a.records += count;
        if constexpr (A::reading) {
            v.resize(count);
        }
        if constexpr (std::is_same_v<V, std::vector<bool>>) {
            for (usize index = 0; index < v.size(); ++index) {
                bool bit = v[index];
                a.number(bit);
                if constexpr (A::reading) {
                    v[index] = bit;
                }
            }
        } else {
            for (auto& entry : v) {
                field(a, entry);
            }
        }
    } else if constexpr (IsOptional<V>::value) {
        bool present = v.has_value();
        a.number(present);
        if constexpr (A::reading) {
            if (present) {
                v.emplace(typename V::value_type{});
            } else {
                v.reset();
            }
        }
        if (present) {
            field(a, *v);
        }
    } else if constexpr (IsArray<V>::value) {
        for (auto& entry : v) {
            field(a, entry);
        }
    } else if constexpr (IsPair<V>::value) {
        fields(a, v.first, v.second);
    } else if constexpr (std::is_same_v<V, StageStatePacket::State>) {
        fields(a, v.world, v.fixtures, v.camera, v.portals, v.transporters);
    } else if constexpr (std::is_same_v<V, LevelTransportersCheckpoint::State>) {
        fields(a, v.catalog, v.tree, v.players, v.collision, v.pads, v.textures,
               v.presentationAdvanced);
    } else if constexpr (std::is_same_v<V, LevelTransportersCheckpoint::Pad>) {
        fields(a, v.source, v.id, v.destinationId);
        optionalIndex(a, v.destination);
        fields(a, v.position, v.radius, v.height, v.transform, v.support, v.supportLocal,
               v.anchorLocal, v.model, v.pose, v.animation, v.previousFrame, v.previousGeneration);
    } else if constexpr (std::is_same_v<V, ExitPortalsCheckpoint::State>) {
        fields(a, v.portals, v.tree, v.sequences, v.waiting, v.visitors, v.collision);
    } else if constexpr (std::is_same_v<V, ExitPortalsCheckpoint::Portal>) {
        fields(a, v.source, v.position, v.transform, v.support, v.supportLocal, v.secret,
               v.minPlayers, v.radius, v.consumed, v.shut, v.alpha, v.departurePosition, v.archive,
               v.model, v.icon, v.tag, v.destination, v.action, v.ticksLeft, v.pose, v.player);
    } else if constexpr (std::is_same_v<V, ExitPortalsCheckpoint::Visitor>) {
        fields(a, v.party, v.ticks);
    } else if constexpr (std::is_same_v<V, TowerCameraCheckpoint::State>) {
        fields(a, v.level, v.markers, v.range, v.camera, v.attention, v.distance, v.marker,
               v.aimedMarker, v.yawRate, v.pitchRate, v.turnStep, v.ringPositions, v.ringDistances,
               v.ringIndex);
    } else if constexpr (std::is_same_v<V, TowerCameraCheckpoint::Marker>) {
        fields(a, v.delay, v.next, v.position, v.rotation);
    } else if constexpr (std::is_same_v<V, CameraRange>) {
        fields(a, v.radiusMin, v.radiusMax, v.minPitch, v.boundsMin, v.boundsMax);
    } else if constexpr (std::is_same_v<V, WorldCamera>) {
        fields(a, v.position, v.pitch, v.yaw, v.roll);
    } else if constexpr (std::is_same_v<V, LevelWorldCheckpoint::State>) {
        fields(a, v.level, v.core, v.triggers, v.destruction, v.falling, v.walls, v.pickups,
               v.rotators, v.textures, v.particles, v.particleSources, v.lighting, v.litNow, v.cues,
               v.playerCount, v.frameRemainder, v.textureAdvance, v.ambientOffset,
               v.particlesAdvanced);
    } else if constexpr (std::is_same_v<V, LevelRef>) {
        fields(a, v.realm, v.realmId, v.name, v.title, v.directory, v.items, v.ownItems, v.index);
    } else if constexpr (std::is_same_v<V, WorldStateCheckpoint::State>) {
        fields(a, v.layout, v.catalog, v.resourceCount, v.placements, v.units, v.slots, v.light,
               v.darken, v.textureFrame, v.moving, v.hidden, v.contactOnly, v.blockedFloorExits,
               v.animations, v.cycles);
    } else if constexpr (std::is_same_v<V, WorldStateCheckpoint::Placement>) {
        fields(a, v.local, v.previous, v.continuity);
    } else if constexpr (std::is_same_v<V, WorldStateCheckpoint::Unit>) {
        fields(a, v.object, v.alpha, v.visible);
    } else if constexpr (std::is_same_v<V, WorldStateCheckpoint::Slot>) {
        fields(a, v.index, v.texture, v.frame, v.cycle, v.cyclePosition, v.cycleSpeed, v.offset,
               v.scrollPhase, v.scrollVelocity, v.usable, v.thermal);
    } else if constexpr (std::is_same_v<V, WorldStateCheckpoint::Light>) {
        fields(a, v.ambient, v.color, v.direction);
        // Native gatherLights overwrites all three Stage point-light copies at draw.
        drawCache(a, v.points);
    } else if constexpr (std::is_same_v<V, WorldStateCheckpoint::Point>) {
        fields(a, v.position, v.color, v.radius, v.intensity);
    } else if constexpr (std::is_same_v<V, WorldStateCheckpoint::Moving>) {
        fields(a, v.object, v.transform);
    } else if constexpr (std::is_same_v<V, WorldStateCheckpoint::Cycle>) {
        fields(a, v.object, v.wrapped);
    } else if constexpr (std::is_same_v<V, WorldAnimator::Cursor>) {
        fields(a, v.object, v.frames, v.frame, v.reverse, v.once, v.finished, v.held,
               v.presentationCut);
    } else if constexpr (std::is_same_v<V, LevelWorldCheckpoint::Cue>) {
        field(a, v.kind);
        index(a, v.rotator);
        field(a, v.position);
    } else if constexpr (std::is_same_v<V, LevelTriggers::Checkpoint>) {
        fields(a, v.triggers, v.targets, v.parents, v.figures, v.refusals, v.lessons, v.openings,
               v.settled, v.cameraCues, v.wakes, v.frameRemainder, v.emptyToggleDelay,
               v.playerCount, v.cameraHeld);
    } else if constexpr (std::is_same_v<V, LevelTrigger>) {
        fields(a, v.instance, v.spot, v.target, v.id, v.nextId, v.refusalCooldown, v.toggleCooldown,
               v.next, v.chained, v.flags, v.kind, v.radius, v.sound, v.fired, v.shootable,
               v.height, v.shot, v.occupied, v.movementLesson, v.forced, v.minPlayers, v.enabled,
               v.heldContacts, v.floor, v.localPlacement, v.placement);
    } else if constexpr (std::is_same_v<V, LevelTriggers::Checkpoint::Target>) {
        fields(a, v.subtype, v.object, v.kind, v.animated, v.open, v.settled, v.pressed,
               v.wholePartyReady, v.returning, v.forced, v.alpha, v.origin, v.height,
               v.closedHeight, v.openHeight);
        indices(a, v.subtree);
        fields(a, v.spot, v.sound);
    } else if constexpr (std::is_same_v<V, TriggerRefusal>) {
        fields(a, v.trigger, v.id, v.crystals);
    } else if constexpr (std::is_same_v<V, TriggerLesson>) {
        fields(a, v.party, v.platform);
    } else if constexpr (std::is_same_v<V, TriggerOpening>) {
        fields(a, v.target, v.spot, v.fades, v.atOnce, v.sound, v.subtype, v.closed);
    } else if constexpr (std::is_same_v<V, TriggerCameraCue>) {
        fields(a, v.id, v.target, v.shakes);
    } else if constexpr (std::is_same_v<V, WorldDestructionCheckpoint::State>) {
        fields(a, v.objects, v.destroyed);
        indices(a, v.controlled);
        field(a, v.explosions);
    } else if constexpr (std::is_same_v<V, WorldDestructionCheckpoint::Object> ||
                         std::is_same_v<V, WorldObject>) {
        fields(a, v.name, v.position, v.flags, v.objectFlags, v.next, v.child, v.parent, v.radius,
               v.noCollision);
    } else if constexpr (std::is_same_v<V, FallingSceneryCheckpoint::State>) {
        fields(a, v.pieces, v.breakSound, v.leafSound, v.remainder, v.updateSeconds, v.bottom,
               v.bound);
    } else if constexpr (std::is_same_v<V, FallingSceneryCheckpoint::Piece>) {
        fields(a, v.source, v.position, v.rotation, v.velocity, v.previousPosition,
               v.previousRotation, v.visible, v.gravityStep, v.spin, v.subtype, v.minPlayers,
               v.radius, v.height, v.centre, v.shown, v.started);
    } else if constexpr (std::is_same_v<V, DestructibleWallsCheckpoint::State>) {
        fields(a, v.walls, v.bound, v.whiteTexture);
    } else if constexpr (std::is_same_v<V, DestructibleWallsCheckpoint::Wall>) {
        fields(a, v.source, v.transform, v.collisionCentre, v.surface, v.bounds, v.health, v.armor,
               v.minPlayers, v.object, v.shown, v.flash, v.blinks, v.blinkTicks, v.white, v.model);
    } else if constexpr (std::is_same_v<V, DestructibleWallsCheckpoint::Bounds>) {
        fields(a, v.id, v.base, v.radius, v.height, v.node, v.targetScoreScale, v.maxTargetDistance,
               v.potionBottle, v.acquisition);
    } else if constexpr (std::is_same_v<V, DestructibleWallsCheckpoint::Acquisition>) {
        fields(a, v.point, v.radius, v.distanceScale, v.maxHeight, v.enabled);
    } else if constexpr (std::is_same_v<V, PlacedItemsCheckpoint::State>) {
        fields(a, v.definitions, v.placements, v.archives, v.figures, v.bursts, v.items, v.effects,
               v.particles, v.particleResources, v.textures, v.players, v.revealTime, v.revealing,
               v.capturePending, v.burstsAdvanced, v.layout, v.collision);
    } else if constexpr (std::is_same_v<V, PlacedItemsCheckpoint::Resource>) {
        fields(a, v.name, v.archive);
    } else if constexpr (std::is_same_v<V, PlacedItemsCheckpoint::Pose>) {
        fields(a, v.resource, v.pose);
    } else if constexpr (std::is_same_v<V, PlacedItemsCheckpoint::Floor> ||
                         std::is_same_v<V, ChestsCheckpoint::Floor>) {
        fields(a, v.object, v.local);
    } else if constexpr (std::is_same_v<V, PlacedItemsCheckpoint::Item>) {
        fields(a, v.resource, v.floor, v.instance, v.info, v.subtype, v.value, v.health, v.flags,
               v.strength, v.minPlayers, v.radius, v.height, v.position, v.transform, v.particles,
               v.pose, v.animation, v.alpha, v.visible, v.taken, v.contained, v.velocity, v.thrown,
               v.carried, v.opener, v.noGrabSeconds, v.previousTransform, v.previousFrame,
               v.previousAlpha, v.previousGeneration, v.presentationCaptured, v.continuity);
        drawCache(a, v.presentationPose);
    } else if constexpr (std::is_same_v<V, PlacedItemsCheckpoint::Effect>) {
        fields(a, v.resource, v.position, v.animation, v.pose, v.emitters, v.secondsLeft,
               v.emitting);
    } else if constexpr (std::is_same_v<V, RotatorsCheckpoint::State>) {
        fields(a, v.rotators, v.bound);
    } else if constexpr (std::is_same_v<V, RotatorsCheckpoint::Rotator>) {
        fields(a, v.source, v.target, v.cursor, v.subtype, v.speed, v.limit, v.origin, v.spot,
               v.radius, v.height, v.pad);
    } else if constexpr (std::is_same_v<V, Rotators::Cursor>) {
        fields(a, v.instance, v.object, v.turned, v.started, v.done);
    } else if constexpr (std::is_same_v<V, fixture_checkpoint::Placement>) {
        index(a, v.index);
        fields(a, v.info, v.instance);
    } else if constexpr (std::is_same_v<V, ItemInfo>) {
        fields(a, v.type, v.subtype, v.name, v.radius, v.height, v.xSize, v.zSize, v.collisionType,
               v.collisionFlags, v.collisionOffset, v.objectFlags, v.properties, v.value, v.armor,
               v.hitPoints, v.activeType, v.activeOff, v.activeOn, v.choices);
    } else if constexpr (std::is_same_v<V, ItemInstance>) {
        fields(a, v.info, v.minPlayers, v.flags, v.name, v.position, v.rotation, v.params,
               v.collision);
    } else if constexpr (std::is_same_v<V, ItemCollisionTriangle>) {
        fields(a, v.normal, v.vertices);
    } else if constexpr (std::is_same_v<V, CollisionTriangle>) {
        fields(a, v.normal, v.vertices, v.object, v.objectFlags);
    } else if constexpr (std::is_same_v<V, LevelFixturesCheckpoint::State>) {
        fields(a, v.chests, v.gates, v.traps, v.barrels, v.rocks, v.scenery, v.rubble, v.clouds,
               v.blasts);
        indices(a, v.doomedChests);
        field(a, v.difficultyGain);
    } else if constexpr (std::is_same_v<V, LevelFixturesCheckpoint::Cloud>) {
        fields(a, v.position, v.damage, v.secondsLeft, v.effect);
    } else if constexpr (std::is_same_v<V, LevelFixturesCheckpoint::Blast>) {
        fields(a, v.position, v.radius, v.damage, v.seconds, v.elapsed, v.flags, v.started, v.done);
        indices(a, v.players);
        fields(a, v.reached, v.opponents);
        indexTimers(a, v.playerReady);
        field(a, v.opponentReady);
    } else if constexpr (std::is_same_v<V, ChestsCheckpoint::State>) {
        fields(a, v.infos, v.placements, v.chests, v.seed, v.collision, v.layout);
    } else if constexpr (std::is_same_v<V, ChestsCheckpoint::Chest>) {
        fields(a, v.instance, v.info, v.subtype, v.contents, v.count, v.locked, v.state, v.opener,
               v.refusalLeft, v.minPlayers, v.shown, v.held, v.gone, v.revealed, v.previewContents,
               v.wobble, v.preview, v.figure, v.box, v.floor, v.collectedSeconds);
    } else if constexpr (std::is_same_v<V, LockedGatesCheckpoint::State>) {
        fields(a, v.gates, v.collision, v.layout);
    } else if constexpr (std::is_same_v<V, LockedGatesCheckpoint::Gate>) {
        fields(a, v.instance, v.definition, v.placement, v.subtype, v.state, v.openingTicks,
               v.refusalLeft, v.minPlayers, v.shown, v.blocksPassage, v.figure, v.box, v.support);
    } else if constexpr (std::is_same_v<V, TrapsCheckpoint::State>) {
        field(a, v.traps);
        indices(a, v.wakes);
        fields(a, v.gaps, v.random, v.timeScale, v.collision, v.layout);
    } else if constexpr (std::is_same_v<V, TrapsCheckpoint::Trap>) {
        fields(a, v.instance, v.definition, v.placement, v.damage, v.offTime, v.subtype,
               v.properties, v.action, v.ticksLeft, v.minPlayers, v.shown, v.disarmed, v.gone,
               v.rectangularContact, v.figure, v.box, v.support);
    } else if constexpr (std::is_same_v<V, BreakablesCheckpoint::State>) {
        fields(a, v.infos, v.barrels, v.seed, v.collision, v.layout);
    } else if constexpr (std::is_same_v<V, BreakablesCheckpoint::Barrel>) {
        fields(a, v.instance, v.placement, v.kind, v.health, v.armor, v.contents, v.count,
               v.minPlayers, v.state, v.breakingSeconds, v.shown, v.gone, v.radius, v.height,
               v.collisionOffset, v.figure, v.box, v.support);
    } else if constexpr (std::is_same_v<V, SafeRocksCheckpoint::State>) {
        fields(a, v.rocks, v.textureSlots, v.animations);
        index(a, v.resolvedAnimations);
        fields(a, v.textures, v.layout, v.items, v.animationItems);
    } else if constexpr (std::is_same_v<V, SafeRocksCheckpoint::Rock>) {
        fields(a, v.instance, v.definition, v.source, v.health, v.baseHealth, v.armor, v.tier,
               v.minPlayers, v.shown, v.dormant, v.activationDelay, v.position, v.obstacle,
               v.placement, v.models);
    } else if constexpr (std::is_same_v<V, StaticSceneryCheckpoint::State>) {
        fields(a, v.props, v.collision, v.layout);
    } else if constexpr (std::is_same_v<V, StaticSceneryCheckpoint::Prop>) {
        fields(a, v.instance, v.definition, v.placement, v.minPlayers, v.shown, v.figure, v.support,
               v.box);
    } else if constexpr (std::is_same_v<V, RubbleCheckpoint::State>) {
        fields(a, v.resources, v.pieces);
    } else if constexpr (std::is_same_v<V, RubbleCheckpoint::Resource>) {
        fields(a, v.archive, v.object);
    } else if constexpr (std::is_same_v<V, RubbleCheckpoint::Piece>) {
        fields(a, v.resource, v.transform);
    } else if constexpr (std::is_same_v<V, LockedGatesCheckpoint::Support> ||
                         std::is_same_v<V, TrapsCheckpoint::Support> ||
                         std::is_same_v<V, BreakablesCheckpoint::Support> ||
                         std::is_same_v<V, StaticSceneryCheckpoint::Support>) {
        fields(a, v.object, v.local, v.boxOffset, v.boxYaw);
    } else if constexpr (std::is_same_v<V, Obstacle>) {
        fields(a, v.centre, v.yaw, v.halfAcross, v.halfAlong, v.height, v.solid, v.cylinderRadius,
               v.enemyItem);
    } else if constexpr (std::is_same_v<V, Obstacle::ItemQuery>) {
        fields(a, v.cylindrical, v.radius);
    } else if constexpr (std::is_same_v<V, TextureAnimationInfo>) {
        fields(a, v.name, v.frameName, v.texture, v.source, v.frames, v.start, v.rate, v.offset,
               v.flag);
    } else if constexpr (std::is_same_v<V, ItemFigureCheckpoint::State>) {
        fields(a, v.state, v.bound, v.archive);
    } else if constexpr (std::is_same_v<V, ItemFigure::Checkpoint>) {
        fields(a, v.animation, v.pose);
        drawCache(a, v.presentationPose);
        fields(a, v.textures, v.particles, v.poseSequence, v.poseFrame, v.poseGeneration,
               v.continuity, v.previousTransform, v.previousFrame, v.previousGeneration,
               v.presentationCaptured, v.presentationAdvanced, v.textureSequence, v.textureFrame,
               v.gateParticles, v.position, v.yaw, v.transform, v.placement, v.index, v.loop,
               v.holdPose);
    } else if constexpr (std::is_same_v<V, AnimationPlayer::Checkpoint>) {
        if constexpr (A::reading) {
            const auto decoded = AnimationCheckpoint::decode(a.raw(AnimationCheckpoint::kBytes));
            if (!decoded) {
                throw FormatError("invalid stage animation cursor");
            }
            v = *decoded;
        } else {
            const auto encoded = AnimationCheckpoint::encode(v);
            if (!encoded) {
                throw FormatError("invalid stage animation cursor");
            }
            a.raw(*encoded);
        }
    } else if constexpr (std::is_same_v<V, TextureAnimator::Checkpoint>) {
        fields(a, v.frame, v.remainder, v.advance);
    } else if constexpr (std::is_same_v<V, TreePose::Checkpoint>) {
        fields(a, v.tree, v.nodes, v.poses);
    } else if constexpr (std::is_same_v<V, TreePose::Checkpoint::Node>) {
        fields(a, v.name, v.parent, v.rest);
    } else if constexpr (std::is_same_v<V, NodePose>) {
        fields(a, v.rotation, v.position, v.scale, v.pitchYawRoll);
    } else if constexpr (std::is_same_v<V, TreeParticles::Checkpoint>) {
        fields(a, v.field, v.nodes);
    } else if constexpr (std::is_same_v<V, TreeParticles::Binding>) {
        index(a, v.node);
        field(a, v.texture);
    } else if constexpr (std::is_same_v<V, ParticleField::Checkpoint>) {
        fields(a, v.entries, v.frameRemainder, v.lastAdvance, v.heatFrames);
    } else if constexpr (std::is_same_v<V, ParticleField::Checkpoint::Entry>) {
        fields(a, v.emitter, v.worldObject);
    } else if constexpr (std::is_same_v<V, ParticleEmitter::Checkpoint>) {
        fields(a, v.descriptor, v.node, v.particles, v.phase, v.age, v.saved, v.emitting,
               v.spriteScale, v.random);
    } else if constexpr (std::is_same_v<V, Particle>) {
        fields(a, v.origin, v.velocity, v.age);
    } else if constexpr (std::is_same_v<V, ParticleDescriptor>) {
        fields(a, v.delay, v.emitFrames, v.fadeFrames, v.forever, v.oneShot, v.dynamic, v.angle,
               v.direction, v.volume, v.rate, v.rateRandom, v.gravity, v.drag, v.speed,
               v.maxParticles, v.maxDirections, v.maxPositions, v.particleLife, v.particleFade,
               v.red, v.green, v.blue, v.alpha, v.width, v.texture, v.additive, v.multiply,
               v.sorted, v.depthTest, v.depthWrite);
    } else if constexpr (std::is_same_v<V, ParticleEnvelope>) {
        fields(a, v.lifeStart, v.lifeEnd, v.fadeStart, v.fadeEnd);
    } else {
        static_assert(std::is_same_v<V, void>, "stage field lacks explicit codec");
    }
}
bool stageValid(const StageStatePacket::State& state, bool observation = false) {
    return (observation ? state.world.validObservation() : state.world.valid()) &&
           state.fixtures.valid() &&
           (!state.camera || (state.camera->valid() && state.camera->level == state.world.level)) &&
           (!state.portals || state.portals->valid()) &&
           (!state.transporters || state.transporters->valid());
}
// Bind pad resources and ordered authored links, but not their live placement,
// pose or clocks. Historical native support lag remains an exact mutable value.
std::optional<std::array<u8, 32>>
transporterIdentity(const std::optional<LevelTransportersCheckpoint::State>& state,
                    const GeometryCatalog& catalog) {
    try {
        Writer identity(&catalog);
        identity.limit = StageStatePacket::kMaxDecodedBytes;
        identity.number(state.has_value());
        if (state) {
            if (state->pads.size() > LevelTransportersCheckpoint::State::kMaxPads) {
                return std::nullopt;
            }
            fields(identity, state->catalog, state->tree, state->players, state->collision);
            identity.number(static_cast<u32>(state->pads.size()));
            for (const auto& pad : state->pads) {
                fields(identity, pad.source, pad.id, pad.destinationId);
                optionalIndex(identity, pad.destination);
                fields(identity, pad.radius, pad.height, pad.support, pad.supportLocal,
                       pad.anchorLocal, pad.model);
            }
        }
        Sha256 hash;
        hash.update(identity.bytes);
        return hash.finish();
    } catch (const FormatError&) {
        return std::nullopt;
    }
}
bool matches(const StageStatePacket::State& state, const GeometryCatalog& catalog) {
    if (state.world.core.catalog != catalog.core || state.world.level != catalog.level) {
        return false;
    }
    const auto identity = transporterIdentity(state.transporters, catalog);
    return identity && *identity == catalog.transporters;
}
void hashRows(Sha256& hash, const auto& rows) {
    Writer count;
    count.number(static_cast<u32>(rows.size()));
    hash.update(count.bytes);
    for (const auto& row : rows) {
        const auto key = geometryKey(row);
        hash.update({reinterpret_cast<const u8*>(key.data()), key.size()});
    }
}
} // namespace
struct StageStatePacket::Catalog::Data : GeometryCatalog {};
const std::array<u8, 32>& StageStatePacket::Catalog::fingerprint() const {
    static constexpr std::array<u8, 32> empty{};
    return m_data ? m_data->fingerprint : empty;
}
bool StageStatePacket::Catalog::valid() const {
    return m_data != nullptr;
}
std::optional<StageStatePacket::Catalog> StageStatePacket::Catalog::build(const State& trusted) {
    return build(trusted, false);
}
std::optional<StageStatePacket::Catalog>
StageStatePacket::Catalog::buildObservation(const State& trusted) {
    return build(trusted, true);
}
std::optional<StageStatePacket::Catalog> StageStatePacket::Catalog::build(const State& trusted,
                                                                          bool observation) {
    try {
        auto data = std::make_shared<Data>();
        Writer collector(nullptr, data.get());
        field(collector, trusted);
        if (!stageValid(trusted, observation)) {
            return std::nullopt;
        }
        data->core = trusted.world.core.catalog;
        data->level = trusted.world.level;
        const auto transporters = transporterIdentity(trusted.transporters, *data);
        if (!transporters) {
            return std::nullopt;
        }
        data->transporters = *transporters;
        Sha256 hash;
        hash.update(data->core);
        hash.update(data->transporters);
        Writer identity;
        field(identity, data->level);
        hash.update(identity.bytes);
        hashRows(hash, data->items);
        hashRows(hash, data->walls);
        data->fingerprint = hash.finish();
        return Catalog{std::move(data)};
    } catch (const FormatError&) {
        return std::nullopt;
    }
}
bool StageStatePacket::envelopeValid(std::span<const u8> bytes) {
    if (bytes.size() <= kHeaderBytes || bytes.size() > kMaxBytes) {
        return false;
    }
    try {
        Reader reader(bytes);
        u32 magic = 0, version = 0, length = 0, codec = 0, decoded = 0, reserved = 0;
        fields(reader, magic, version, length, codec, decoded, reserved);
        const usize payload = bytes.size() - kHeaderBytes;
        return magic == kMagic && version == kVersion && length == bytes.size() && reserved == 0 &&
               decoded > 0 && decoded <= kMaxDecodedBytes &&
               ((codec == kRaw && payload == decoded) ||
                ((codec == kLz4 || codec == kLz4BytePlanes) && payload < decoded));
    } catch (const FormatError&) {
        return false;
    }
}
std::optional<std::vector<u8>> StageStatePacket::encode(const State& state, const Catalog& catalog,
                                                        Encoding encoding) {
    if (!catalog.valid() || !matches(state, *catalog.m_data) ||
        (encoding != Encoding::Automatic && encoding != Encoding::Raw &&
         encoding != Encoding::Plain && encoding != Encoding::Fast)) {
        return std::nullopt;
    }
    try {
        Writer body(catalog.m_data.get());
        body.limit = kMaxDecodedBytes;
        field(body, state);
        if (!stageValid(state)) {
            return std::nullopt;
        }
        std::vector<u8> compressed;
        u32 codec = kRaw;
        if (encoding != Encoding::Raw && body.bytes.size() >= kCompressionMinimum) {
            compressed = compress(body.bytes);
            if (!compressed.empty()) {
                codec = kLz4;
            }
            const usize plainSize = compressed.empty() ? body.bytes.size() : compressed.size();
            if (encoding == Encoding::Automatic ||
                (encoding == Encoding::Fast && plainSize > kMaxBytes - kHeaderBytes)) {
                auto planar = compress(BytePlanes::encode(body.bytes));
                if (!planar.empty() && (compressed.empty() || planar.size() < compressed.size())) {
                    compressed = std::move(planar);
                    codec = kLz4BytePlanes;
                }
            }
        }
        const auto payload =
            compressed.empty() ? std::span<const u8>{body.bytes} : std::span<const u8>{compressed};
        if (payload.size() > kMaxBytes - kHeaderBytes) {
            return std::nullopt;
        }
        Writer writer;
        writer.word(kMagic);
        writer.word(kVersion);
        writer.word(static_cast<u32>(kHeaderBytes + payload.size()));
        writer.word(codec);
        writer.word(static_cast<u32>(body.bytes.size()));
        writer.word(0);
        writer.raw(catalog.fingerprint());
        writer.raw(payload);
        return std::move(writer.bytes);
    } catch (const FormatError&) {
        return std::nullopt;
    }
}
std::optional<usize> StageStatePacket::encodedSize(const State& state, const Catalog& catalog,
                                                   Encoding encoding) {
    const auto bytes = encode(state, catalog, encoding);
    return bytes ? std::optional{bytes->size()} : std::nullopt;
}
std::optional<usize> StageStatePacket::decodedSize(const State& state, const Catalog& catalog) {
    if (!catalog.valid() || !matches(state, *catalog.m_data)) {
        return std::nullopt;
    }
    try {
        Writer writer(catalog.m_data.get());
        writer.limit = kMaxDecodedBytes;
        writer.countOnly = true;
        field(writer, state);
        if (!stageValid(state)) {
            return std::nullopt;
        }
        return writer.used;
    } catch (const FormatError&) {
        return std::nullopt;
    }
}
std::optional<std::array<u8, 32>> StageStatePacket::canonicalDigest(const State& state,
                                                                    const Catalog& catalog,
                                                                    bool presentationIndependent) {
    if (!catalog.valid() || !matches(state, *catalog.m_data)) {
        return std::nullopt;
    }
    try {
        CanonicalDigestSink sink;
        Writer writer(catalog.m_data.get());
        writer.limit = kMaxDecodedBytes;
        writer.digest = &sink;
        writer.agreement = presentationIndependent;
        field(writer, state);
        return sink.finish();
    } catch (const FormatError&) {
        return std::nullopt;
    }
}

std::optional<StageStatePacket::State> StageStatePacket::decode(std::span<const u8> bytes,
                                                                const Catalog& catalog) {
    if (!catalog.valid() || !envelopeValid(bytes) ||
        !std::ranges::equal(bytes.subspan(24, 32), catalog.fingerprint())) {
        return std::nullopt;
    }
    try {
        ByteReader header(bytes.subspan(12, 8));
        const u32 codec = header.readU32();
        const u32 decoded = header.readU32();
        auto payload = bytes.subspan(kHeaderBytes);
        std::vector<u8> unpacked;
        if (codec == kLz4 || codec == kLz4BytePlanes) {
            // The complete envelope and trusted fingerprint are checked before
            // this bounded allocation. Nested budgets still apply after decode.
            unpacked.resize(decoded);
            // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
            const s32 written =
                LZ4_decompress_safe(reinterpret_cast<const char*>(payload.data()),
                                    reinterpret_cast<char*>(unpacked.data()),
                                    static_cast<s32>(payload.size()), static_cast<s32>(decoded));
            // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
            if (written != static_cast<s32>(decoded)) {
                return std::nullopt;
            }
            if (codec == kLz4BytePlanes) {
                unpacked = BytePlanes::decode(unpacked);
            }
            payload = unpacked;
        }
        Reader reader(payload, catalog.m_data.get());
        State state;
        field(reader, state);
        if (reader.remaining() != 0 || !matches(state, *catalog.m_data) || !stageValid(state)) {
            return std::nullopt;
        }
        return state;
    } catch (const FormatError&) {
        return std::nullopt;
    }
}
} // namespace gdl::game
