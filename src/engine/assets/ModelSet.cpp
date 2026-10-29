#include "engine/assets/ModelSet.h"

#include <cmath>
#include <exception>

#include "engine/core/Assert.h"
#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/platform/Paths.h"

#include "formats/GeometryStream.h"

namespace gdl {

ModelSet ModelSet::fromDecoded(std::vector<ModelSetEntry> entries, std::vector<Mesh> meshes) {
    if (entries.size() != meshes.size()) {
        throw FormatError("decoded model metadata and mesh counts differ");
    }
    ModelSet result;
    for (usize i = 0; i < entries.size(); ++i) {
        auto& entry = entries[i];
        entry.name = normalizeAssetName(entry.name);
        if (!entry.name.empty() &&
            !result.m_byName.try_emplace(entry.name, static_cast<u32>(i)).second) {
            throw FormatError("duplicate decoded model name: " + entry.name);
        }
        entry.triangles = 0;
        for (const auto& part : meshes[i].parts) {
            if (part.indices.size() % 3 != 0) {
                throw FormatError("decoded model has an incomplete triangle");
            }
            for (const auto index : part.indices) {
                if (index >= meshes[i].vertices.size()) {
                    throw FormatError("decoded model vertex index out of range");
                }
            }
            entry.triangles += static_cast<u32>(part.indices.size() / 3);
        }
        for (const auto& vertex : meshes[i].vertices) {
            for (s32 axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(vertex.position[axis]) || !std::isfinite(vertex.normal[axis])) {
                    throw FormatError("non-finite decoded model geometry");
                }
            }
            if (!std::isfinite(vertex.uv.x) || !std::isfinite(vertex.uv.y)) {
                throw FormatError("non-finite decoded model texture coordinate");
            }
        }
    }
    result.m_entries = std::move(entries);
    result.m_meshes = std::move(meshes);
    result.m_loaded.assign(result.m_entries.size(), true);
    return result;
}

bool ModelSet::load(const std::filesystem::path& directory) {
    m_entries.clear();
    m_byName.clear();
    m_meshes.clear();
    m_loaded.clear();
    m_native.reset();
    if (const auto file = AssetLocator(directory).find("objects.ngc")) {
        return loadNative(*file);
    }
    log::warn("Model set {}: objects.ngc is missing", paths::utf8(directory));
    return false;
}

const ModelSetEntry& ModelSet::entry(u32 index) const {
    GDL_VERIFY(index < m_entries.size(), "model index out of range");
    return m_entries[index];
}

std::optional<u32> ModelSet::find(std::string_view name) const {
    const auto it = m_byName.find(normalizeAssetName(name));
    if (it == m_byName.end()) {
        return std::nullopt;
    }
    return it->second;
}

const Mesh& ModelSet::mesh(u32 index) {
    GDL_VERIFY(index < m_entries.size(), "model index out of range");
    if (!m_loaded[index]) {
        if (!m_native) {
            throw FormatError("model archive is not loaded");
        }
        Mesh decoded;
        decoded.flags = m_native->objects()[index].flags;
        for (const auto& sub : m_native->objects()[index].subObjects) {
            formats::decodeGeometryStream(sub.geometry, sub.textureIndex, sub.lightmapIndex,
                                          decoded);
        }
        m_meshes[index] = std::move(decoded);
        m_loaded[index] = true;
    }
    return m_meshes[index];
}

bool ModelSet::loadNative(const std::filesystem::path& file) {
    try {
        m_native = formats::ModelArchive::parse(readFile(file));
        m_entries.resize(m_native->objects().size());
        for (const auto& def : m_native->objectDefs()) {
            if (def.objectIndex < 0 || static_cast<usize>(def.objectIndex) >= m_entries.size()) {
                throw FormatError("object definition index out of range");
            }
            const auto index = static_cast<u32>(def.objectIndex);
            const auto name = normalizeAssetName(def.name);
            if (m_entries[index].name.empty()) {
                m_entries[index].name = name;
            }
            m_byName.try_emplace(name, index);
        }
        for (usize i = 0; i < m_entries.size(); ++i) {
            const auto count = m_native->objects()[i].triangleCount;
            m_entries[i].triangles = count > 0 ? static_cast<u32>(count) : 0;
            m_entries[i].flags = m_native->objects()[i].flags;
        }
        m_meshes.resize(m_entries.size());
        m_loaded.assign(m_entries.size(), false);
        return !m_entries.empty();
    } catch (const std::exception& e) {
        log::warn("Native model set {}: {}", paths::utf8(file), e.what());
        m_native.reset();
        m_entries.clear();
        m_byName.clear();
        return false;
    }
}

} // namespace gdl
