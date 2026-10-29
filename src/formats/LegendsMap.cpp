#include "formats/LegendsMap.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <string_view>
#include <utility>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

#include "formats/LegendsN64Scene.h"

namespace gdl::formats {
namespace {

constexpr usize kMaxObjects = 16384;
constexpr usize kMaxVertices = 1000000;
constexpr usize kMaxCommands = 1000000;
constexpr u32 kEmptyVertex = std::numeric_limits<u32>::max();

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw FormatError("N64 Legends map: " + std::string(message));
    }
}

void range(std::span<const u8> bytes, usize offset, usize length) {
    require(offset <= bytes.size() && length <= bytes.size() - offset, "truncated data");
}

f32 real(std::span<const u8> bytes, usize offset) {
    const f32 value = std::bit_cast<f32>(readU32BE(bytes, offset));
    require(std::isfinite(value), "non-finite coordinate");
    return value;
}

struct Section {
    usize offset = 0;
    usize count = 0;
    usize end = 0;
};

/** N64's WAD has an eight-byte header and twelve-byte big-endian directory entries.
 * Counts are section-specific, not universally byte lengths. */
class NativeWad {
public:
    explicit NativeWad(std::span<const u8> bytes) : m_bytes(bytes) {
        range(bytes, 0, 8);
        const usize directory = readU32BE(bytes, 0);
        const usize count = readU32BE(bytes, 4);
        require(directory >= 8 && count <= 4096, "invalid WAD directory");
        range(bytes, directory, count * 12);
        for (usize i = 0; i < count; ++i) {
            const usize at = directory + i * 12;
            const std::string tag(std::bit_cast<const char*>(bytes.data() + at), 4);
            const usize offset = readU32BE(bytes, at + 4);
            const usize size = readU32BE(bytes, at + 8);
            require(offset >= 8 && offset <= directory, "invalid WAD section offset");
            require(m_sections.emplace(tag, Section{offset, size, directory}).second,
                    "duplicate WAD section");
        }
        for (auto& [name, section] : m_sections) {
            for (const auto& [otherName, other] : m_sections) {
                if (other.offset > section.offset) {
                    section.end = std::min(section.end, other.offset);
                }
            }
        }
    }

    const Section& section(std::string_view tag, usize stride) const {
        const auto found = m_sections.find(std::string(tag));
        require(found != m_sections.end(), "missing " + std::string(tag));
        const Section& value = found->second;
        require(stride != 0 && value.count <= (value.end - value.offset) / stride,
                "invalid " + std::string(tag) + " count");
        return value;
    }

    const Section* find(std::string_view tag) const {
        const auto found = m_sections.find(std::string(tag));
        return found == m_sections.end() ? nullptr : &found->second;
    }

    std::string name(const Section& section, usize offset) const {
        require(offset < section.count, "name outside string table");
        const usize begin = section.offset + offset;
        usize end = begin;
        while (end < section.offset + section.count && m_bytes[end] != 0) {
            ++end;
        }
        require(end != section.offset + section.count, "unterminated name");
        return {std::bit_cast<const char*>(m_bytes.data() + begin), end - begin};
    }

private:
    std::span<const u8> m_bytes;
    std::map<std::string, Section> m_sections;
};

struct NativeModel {
    u32 flags = 0;
    usize displayList = 0;
    usize vertices = 0;
};

void decodeCollision(const NativeWad& world, std::span<const u8> bytes, LegendsMap& result) {
    if (world.find("CTRI") == nullptr && world.find("CVTX") == nullptr) {
        result.warnings.emplace_back("Native collision is absent; this map is survey-only.");
        return;
    }
    const Section& triangles = world.section("CTRI", 8);
    const Section& vertices = world.section("CVTX", 6);
    const Section& grid = world.section("GRID", 2);
    const Section& lists = world.section("CLST", 1);
    const Section* fixedPoint = world.find("CFOT");
    // US game.bin e003a44c uses CFOT's directory count as a divisor, not as a
    // section length, then truncates converted coordinates into 1/32-unit shorts.
    const usize divisor = fixedPoint == nullptr ? 64 : fixedPoint->count;
    require(divisor != 0 && divisor <= 65536, "invalid collision fixed-point divisor");
    require(triangles.count <= 32768 && vertices.count <= 32768 && grid.count <= kMaxVertices &&
                lists.count <= kMaxVertices && lists.count % 2 == 0,
            "invalid collision table count");
    std::vector<Vec3> positions;
    positions.reserve(vertices.count);
    for (usize i = 0; i < vertices.count; ++i) {
        Vec3 position;
        for (int axis = 0; axis < 3; ++axis) {
            const s16 native = static_cast<s16>(
                readU16BE(bytes, vertices.offset + i * 6 + static_cast<usize>(axis) * 2));
            const f32 converted = static_cast<f32>(native) * 32.0f / static_cast<f32>(divisor);
            require(converted >= -32768.0f && converted <= 32767.0f,
                    "collision coordinate overflows native fixed point");
            position[axis] = static_cast<f32>(static_cast<s16>(converted)) / 32.0f;
        }
        positions.push_back(position);
    }

    // e003faf4 walks GRID offsets into CLST. F000|index selects a WOBJ owner;
    // 8000 on a triangle ends the cell. No marker means static world geometry.
    constexpr s32 kUnreferenced = -2;
    std::vector<s32> owners(triangles.count, kUnreferenced);
    usize remainingReferences = kMaxCommands * 8;
    for (usize i = 0; i < grid.count; ++i) {
        const s16 first = static_cast<s16>(readU16BE(bytes, grid.offset + i * 2));
        require(first >= -1, "negative collision-list offset");
        if (first == -1) {
            continue;
        }
        s32 owner = -1;
        auto cursor = static_cast<usize>(first);
        for (;;) {
            require(cursor < lists.count / 2 && remainingReferences != 0,
                    "unterminated collision cell");
            --remainingReferences;
            const u16 entry = readU16BE(bytes, lists.offset + cursor * 2);
            ++cursor;
            if ((entry & 0xf000U) == 0xf000U) {
                owner = static_cast<s32>(entry & 0xfffU);
                require(static_cast<usize>(owner) < result.objects.size(),
                        "collision owner outside WOBJ");
            } else {
                const usize index = entry & 0x7fffU;
                require(index < owners.size(), "collision-list triangle outside CTRI");
                require(owners[index] == kUnreferenced || owners[index] == owner,
                        "conflicting collision owners");
                owners[index] = owner;
                if ((entry & 0x8000U) != 0) {
                    break;
                }
            }
        }
    }
    result.collision.reserve(triangles.count);
    for (usize i = 0; i < triangles.count; ++i) {
        const usize at = triangles.offset + i * 8;
        LegendsMapCollisionTriangle triangle;
        triangle.flags = readU16BE(bytes, at + 6);
        triangle.nativeIndex = static_cast<u32>(i);
        triangle.owner = owners[i] == kUnreferenced ? -1 : owners[i];
        for (usize vertex = 0; vertex < 3; ++vertex) {
            const usize index = readU16BE(bytes, at + vertex * 2);
            require(index < positions.size(), "collision vertex outside CVTX");
            triangle.vertices[vertex] = positions[index];
        }
        // e003da64 forms (v1-v0) cross (v2-v0), independently of render vertices.
        const Vec3 normal = glm::cross(triangle.vertices[1] - triangle.vertices[0],
                                       triangle.vertices[2] - triangle.vertices[0]);
        const f32 squaredLength = glm::dot(normal, normal);
        require(squaredLength >= 0.01f, "degenerate collision triangle");
        triangle.normal = normal / std::sqrt(squaredLength);
        const u32 ownerFlags =
            triangle.owner < 0 ? 0U : result.objects[static_cast<usize>(triangle.owner)].flags;
        // Native movement e0040958 queries 1D0 for ground and 220 for walls,
        // excluding owners with 40000. Unreferenced data never enters that query.
        const bool active = owners[i] != kUnreferenced && (ownerFlags & 0x40000U) == 0;
        triangle.floor = active && (triangle.flags & 0x1d0U) != 0;
        triangle.wall = active && (triangle.flags & 0x220U) != 0;
        result.collision.push_back(triangle);
    }
}

void decodeStarts(std::span<const u8> bytes, LegendsMap& result) {
    if (bytes.empty()) {
        return;
    }
    range(bytes, 0, 92);
    const usize offset = readU32BE(bytes, 20);
    const usize count = readU16BE(bytes, 48);
    require(offset >= 92, "placement markers overlap header");
    range(bytes, offset, count * 8);
    for (usize i = 0; i < count; ++i) {
        const usize at = offset + i * 8;
        // e006eb24 expands these XYZ quarters; e006ebd0 selects type 20 as
        // the player entrance. The record has no rotation. Runtime grounding
        // is deliberately left to traversal, not baked into the authored marker.
        if (bytes[at + 6] != 0x20) {
            continue;
        }
        LegendsMapStart start;
        for (int axis = 0; axis < 3; ++axis) {
            start.position[axis] = static_cast<f32>(static_cast<s16>(
                                       readU16BE(bytes, at + static_cast<usize>(axis) * 2))) /
                                   4.0f;
        }
        start.nativeIndex = static_cast<u32>(i);
        start.flags = bytes[at + 7];
        result.starts.push_back(start);
    }
}

Mesh decodeGeometry(std::span<const u8> bytes, const NativeModel& model, const Section& geometry,
                    const std::map<u32, u32>& materials, usize materialCount,
                    usize& remainingVertices, usize& remainingCommands,
                    bool runtimeMaterials = false, std::vector<u32>* vertexSources = nullptr,
                    std::vector<LegendsN64ColorRamp>* colorRamps = nullptr) {
    Mesh result;
    // e001ea34 tests LOD flag 2 before selecting the RSP lighting path. The
    // final four vertex bytes are signed XYZ normals + alpha in that path,
    // not baked RGB; flag 4 and ordinary unlit objects retain vertex colours.
    result.prelit = (model.flags & 2U) == 0;
    if (model.displayList == 0) {
        return result;
    }
    require(model.displayList >= geometry.offset && model.displayList < geometry.end,
            "display list outside OBJS");
    require(model.vertices >= geometry.offset && model.vertices <= geometry.end,
            "vertex base outside OBJS");
    std::array<u32, 32> cache{};
    cache.fill(kEmptyVertex);
    std::array<u32, 16> textureSegments{};
    textureSegments.fill(kEmptyVertex);
    u32 material = 0;
    std::optional<Color> primitive;
    std::optional<Color> environment;
    bool colorRamp = false;
    std::optional<LegendsN64ColorRamp> previousRamp;
    const auto triangle = [&](u32 word) {
        std::optional<LegendsN64ColorRamp> ramp;
        if (colorRamp) {
            require(primitive && environment, "color ramp has no authored endpoints");
            ramp = LegendsN64ColorRamp{0, *primitive, *environment};
        }
        if (result.parts.empty() || result.parts.back().texture != material ||
            previousRamp != ramp) {
            result.parts.push_back(MeshPart{material, 0, {}});
            previousRamp = ramp;
            if (ramp && colorRamps != nullptr) {
                ramp->part = static_cast<u32>(result.parts.size() - 1);
                colorRamps->push_back(*ramp);
            }
        }
        for (const u32 shift : {16U, 8U, 0U}) {
            const u32 encoded = (word >> shift) & 0xffU;
            const usize index = encoded / 2;
            require((encoded & 1U) == 0 && index < cache.size() && cache[index] != kEmptyVertex,
                    "triangle references an unloaded vertex");
            result.parts.back().indices.push_back(cache[index]);
        }
    };
    for (usize at = model.displayList; at <= geometry.end && geometry.end - at >= 8; at += 8) {
        require(remainingCommands != 0, "display-list command budget exceeded");
        --remainingCommands;
        const u32 first = readU32BE(bytes, at);
        const u32 second = readU32BE(bytes, at + 4);
        switch (first >> 24U) {
        case 0x01: { // F3DEX2 vertex cache upload; segment 14 is this object's vertex pool.
            const usize count = (first >> 12U) & 0xffU;
            const usize end = (first >> 1U) & 0x7fU;
            require(count != 0 && count <= end && end <= cache.size(),
                    "invalid vertex cache upload");
            require((second >> 24U) == 14, "unsupported vertex segment");
            const usize offset = second & 0xffffffU;
            require(offset <= geometry.end - model.vertices, "vertex offset outside OBJS");
            const usize vertices = model.vertices + offset;
            require(count <= (geometry.end - vertices) / 16, "truncated vertex upload");
            require(count <= remainingVertices, "vertex budget exceeded");
            remainingVertices -= count;
            for (usize i = 0; i < count; ++i) {
                const usize vertex = vertices + i * 16;
                MeshVertex value;
                // Model coordinates use six fractional bits. Across the native map corpus,
                // this scale and MTRX placement reproduce the authored WOBJ bounds.
                value.position = Vec3{static_cast<s16>(readU16BE(bytes, vertex)),
                                      static_cast<s16>(readU16BE(bytes, vertex + 2)),
                                      static_cast<s16>(readU16BE(bytes, vertex + 4))} /
                                 64.0f;
                value.uv = Vec2{static_cast<s16>(readU16BE(bytes, vertex + 8)),
                                static_cast<s16>(readU16BE(bytes, vertex + 10))} /
                           32.0f;
                if (result.prelit) {
                    value.color = Color::rgba(bytes[vertex + 12], bytes[vertex + 13],
                                              bytes[vertex + 14], bytes[vertex + 15]);
                } else {
                    value.normal = Vec3{static_cast<s8>(bytes[vertex + 12]),
                                        static_cast<s8>(bytes[vertex + 13]),
                                        static_cast<s8>(bytes[vertex + 14])} /
                                   127.0f;
                    value.color = Color::rgba(255, 255, 255, bytes[vertex + 15]);
                }
                cache[end - count + i] = static_cast<u32>(result.vertices.size());
                result.vertices.push_back(value);
                if (vertexSources != nullptr) {
                    vertexSources->push_back(static_cast<u32>((vertex - model.vertices) / 16));
                }
            }
            break;
        }
        case 0x05: triangle(first); break;
        case 0x06:
            triangle(first);
            triangle(second);
            break;
        case 0xdb: {
            // F3DEX2 gSPSegment is MOVEWORD(index6, segment*4, address).
            // Sumner's archive binds its TXHD texture-load lists to segments
            // 1..4 before DE calls. Resolve only known material entry points;
            // do not execute arbitrary lists or replace drawable segments13/14.
            const u32 offset = first & 0xffffU;
            require((first & 0xffff0000U) == 0xdb060000U && (offset & 3U) == 0 && offset >= 4 &&
                        offset < 13 * 4,
                    "unsupported display-list segment binding");
            const auto found = materials.find(second);
            require(found != materials.end(), "segment binding is not a texture entry point");
            textureSegments[offset / 4] = found->second;
            break;
        }
        case 0xde: {
            require(first == 0xde000000U, "unsupported display-list branch");
            const auto found = materials.find(second);
            if (found != materials.end()) {
                material = found->second;
            } else if ((second >> 24U) < textureSegments.size() && (second & 0xffffffU) == 0 &&
                       textureSegments[second >> 24U] != kEmptyVertex) {
                material = textureSegments[second >> 24U];
            } else if (second == 0x0d000000U && runtimeMaterials) {
                // e001ea34 binds segment13 from drawable+5c. e00259d0 supplies
                // that list from the node's animated texture; it is not slot0.
                material = LegendsN64Model::kRuntimeTexture;
            } else {
                // US game.bin e0026560 splits texture identity into archive=id>>10 and
                // slot=id&0x3ff. Survey calls may only name this archive, never alias
                // another archive's texture into a numerically matching local slot.
                require((second & 0xfffffc00U) == 0x08000000U && (second & 0x3ffU) < materialCount,
                        "unsupported display-list call");
                material = second & 0x3ffU;
            }
            break;
        }
        case 0xdf: return result;
        case 0xfa:
            primitive = Color::rgba(second >> 24U, (second >> 16U) & 255U, (second >> 8U) & 255U,
                                    second & 255U);
            break;
        case 0xfb:
            environment = Color::rgba(second >> 24U, (second >> 16U) & 255U, (second >> 8U) & 255U,
                                      second & 255U);
            break;
        case 0xfc:
            // RDP cycle0=(ENV-PRIM)*TEXEL0+PRIM, alpha=TEXEL0.a;
            // cycle1 passes COMBINED through. In particular, FBAL's raw blue
            // vertex bytes are not a tint under this native combiner.
            colorRamp = first == 0xfc50ffffU && second == 0x3ffdf238U;
            break;
        case 0xd7: // Texture state
        case 0xd9: // Geometry mode
        case 0xe2: // Render mode
        case 0xe3: // Other mode
        case 0xe7: // Pipeline sync
        case 0xf9: // Blend colour
            break; // Material conversion is explicitly outside this geometry survey.
        default:
            throw FormatError("N64 Legends map: unsupported geometry opcode " +
                              std::to_string(first >> 24U));
        }
    }
    throw FormatError("N64 Legends map: unterminated display list");
}

} // namespace

std::vector<LegendsN64Model> decodeLegendsN64Models(std::span<const u8> bytes) {
    const NativeWad wad(bytes);
    const Section& names = wad.section("NAME", 1);
    const Section& headers = wad.section("OBHD", 56);
    const Section& textures = wad.section("TXHD", 24);
    const Section& geometry = wad.section("OBJS", 1);
    require(headers.count <= kMaxObjects && textures.count <= 1024,
            "object archive budget exceeded");
    std::map<u32, u32> materialLists;
    for (usize i = 0; i < textures.count; ++i) {
        materialLists.try_emplace(readU32BE(bytes, textures.offset + i * 24 + 4),
                                  static_cast<u32>(i));
    }
    usize remainingVertices = kMaxVertices;
    usize remainingCommands = kMaxCommands;
    std::vector<LegendsN64Model> result;
    result.reserve(headers.count);
    for (usize i = 0; i < headers.count; ++i) {
        const usize at = headers.offset + i * 56;
        LegendsN64Model model;
        const usize nameOffset = readU16BE(bytes, at + 4);
        model.name = wad.name(names, nameOffset);
        // e001cfc0 obtains the part name by adding signed OBHD byte6 to the
        // full name. e0027210 checks this alias before the complete name.
        const usize prefix = bytes[at + 6];
        require(prefix < 128, "invalid object name prefix");
        // A few renamed retail weapon/static models retain an old prefix that
        // crosses their first terminator. Preserve the native bounded table
        // alias rather than rejecting otherwise valid source artwork.
        model.partName = wad.name(names, nameOffset + prefix);
        const NativeModel native{readU32BE(bytes, at + 8), readU32BE(bytes, at + 12),
                                 readU32BE(bytes, at + 16)};
        model.flags = native.flags;
        if (native.displayList != 0) {
            model.mesh = decodeGeometry(bytes, native, geometry, materialLists, textures.count,
                                        remainingVertices, remainingCommands, true,
                                        &model.vertexSources, &model.colorRamps);
        }
        result.push_back(std::move(model));
    }
    return result;
}

LegendsMap LegendsMap::parseN64(std::span<const u8> worlds, std::span<const u8> models,
                                std::span<const u8> placementBytes) {
    const NativeWad world(worlds);
    const NativeWad objects(models);
    const Section& header = world.section("WHDR", 1);
    require(header.count == 36, "unexpected world header size");
    const Section& placements = world.section("WOBJ", 44);
    const Section& matrices = world.section("MTRX", 48);
    const Section& worldNames = world.section("WONM", 1);
    const Section& modelNames = objects.section("NAME", 1);
    const Section& objectHeaders = objects.section("OBHD", 56);
    const Section& textures = objects.section("TXHD", 24);
    const Section& geometry = objects.section("OBJS", 1);
    require(placements.count <= kMaxObjects && objectHeaders.count <= kMaxObjects,
            "object budget exceeded");

    LegendsMap result;
    for (int axis = 0; axis < 3; ++axis) {
        result.minBounds[axis] = real(worlds, header.offset + static_cast<usize>(axis) * 4);
        result.maxBounds[axis] = real(worlds, header.offset + 12 + static_cast<usize>(axis) * 4);
        require(result.minBounds[axis] <= result.maxBounds[axis], "inverted world bounds");
    }
    std::map<std::string, NativeModel> nativeModels;
    for (usize i = 0; i < objectHeaders.count; ++i) {
        const usize at = objectHeaders.offset + i * 56;
        const std::string name = objects.name(modelNames, readU16BE(models, at + 4));
        const NativeModel value{readU32BE(models, at + 8), readU32BE(models, at + 12),
                                readU32BE(models, at + 16)};
        require(nativeModels.emplace(name, value).second, "duplicate model name");
    }
    std::map<u32, u32> materialLists;
    usize externalTextures = 0;
    for (usize i = 0; i < textures.count; ++i) {
        const usize at = textures.offset + i * 24;
        const u32 list = readU32BE(models, at + 4);
        materialLists.try_emplace(list, static_cast<u32>(i));
        // These are the same native storage flags used by LegendsN64Texture. Pixel
        // availability is separate from the geometry and no substitute is fabricated.
        if (readU32BE(models, at) == 0xfffffffeU || (readU32BE(models, at + 8) & 0x80U) != 0) {
            ++externalTextures;
        }
    }
    usize remainingVertices = kMaxVertices;
    usize remainingCommands = kMaxCommands;
    usize withoutMesh = 0;
    result.objects.reserve(placements.count);
    for (usize i = 0; i < placements.count; ++i) {
        const usize at = placements.offset + i * 44;
        LegendsMapObject item;
        item.flags = readU32BE(worlds, at);
        // e003955c passes WOBJ+4 unchanged to e001d24c/e001f594's drawable.
        // Keep these presentation flags apart from the owner collision flags.
        item.nativeDrawFlags = readU32BE(worlds, at + 4);
        item.name = world.name(worldNames, readU16BE(worlds, at + 8));
        const usize matrix = readU16BE(worlds, at + 10);
        require(matrix < matrices.count, "invalid placement matrix index");
        const usize base = matrices.offset + matrix * 48;
        for (int column = 0; column < 3; ++column) {
            for (int row = 0; row < 3; ++row) {
                item.transform[column][row] =
                    real(worlds, base + static_cast<usize>(column * 3 + row) * 4);
                // All 34 extracted level files store an identity basis here. Do not
                // assume a rotation convention until a nonidentity native case is traced.
                require(item.transform[column][row] == (column == row ? 1.0f : 0.0f),
                        "unsupported nonidentity placement basis");
            }
            item.transform[3][column] = real(worlds, base + 36 + static_cast<usize>(column) * 4);
        }
        const auto model = nativeModels.find(item.name);
        if (model != nativeModels.end()) {
            item.mesh = decodeGeometry(models, model->second, geometry, materialLists,
                                       textures.count, remainingVertices, remainingCommands);
        } else {
            ++withoutMesh; // Native hierarchy/group records need not own a mesh.
        }
        result.objects.push_back(std::move(item));
    }
    // e003a1d4 walks node, child subtree, then sibling. Each created drawable
    // becomes the parent of its children, so a moved world object carries their
    // rendered geometry even though collision retains each explicit WOBJ owner.
    // Preserve this relationship without recomposing the imported transforms.
    std::vector<bool> visited(placements.count, false);
    std::vector<std::pair<s32, s32>> pending;
    if (placements.count != 0) {
        pending.emplace_back(0, -1);
    }
    usize visitedCount = 0;
    while (!pending.empty()) {
        const auto [index, parent] = pending.back();
        pending.pop_back();
        require(index >= 0 && static_cast<usize>(index) < placements.count,
                "world hierarchy index outside WOBJ");
        const auto current = static_cast<usize>(index);
        require(!visited[current], "cyclic or multiply-owned world hierarchy");
        visited[current] = true;
        result.objects[current].nativeOrder = static_cast<u32>(visitedCount);
        ++visitedCount;
        result.objects[current].parent = parent;
        const usize at = placements.offset + current * 44;
        const s16 sibling = std::bit_cast<s16>(readU16BE(worlds, at + 36));
        const s16 child = std::bit_cast<s16>(readU16BE(worlds, at + 38));
        if (sibling >= 0) {
            pending.emplace_back(sibling, parent);
        }
        if (child >= 0) {
            pending.emplace_back(child, index);
        }
    }
    require(visitedCount == placements.count, "unreachable world hierarchy record");
    decodeCollision(world, worlds, result);
    decodeStarts(placementBytes, result);
    result.warnings.emplace_back("Geometry reader retains authored initial placement; dynamic "
                                 "rules are bound by the native scene adapter.");
    if (externalTextures != 0) {
        result.warnings.push_back(std::to_string(externalTextures) +
                                  " texture slots require borrowed or dynamic artwork; unavailable "
                                  "slots are untextured.");
    }
    if (withoutMesh != 0) {
        result.warnings.push_back(std::to_string(withoutMesh) +
                                  " world/group records have no same-named model.");
    }
    return result;
}

} // namespace gdl::formats
