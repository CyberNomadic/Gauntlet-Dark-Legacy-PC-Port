#include "game/level/SpawnedObjects.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

#include "engine/core/Log.h"

namespace gdl::game {

namespace {
bool sameIgnoringCase(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}
} // namespace

std::optional<usize> SpawnedObjects::objectNamed(const WorldLayout& layout, std::string_view name) {
    const auto& objects = layout.objects();
    for (usize i = 0; i < objects.size(); ++i) {
        if (sameIgnoringCase(objects[i].name, name)) {
            return i;
        }
    }
    return std::nullopt;
}

TreeInfo SpawnedObjects::treeOf(const WorldLayout& layout, usize index) {
    const auto& objects = layout.objects();
    TreeInfo tree;
    if (index >= objects.size()) {
        return tree;
    }
    tree.name = objects[index].name;
    // The root, then each node's children after it, each at its offset from its parent.
    std::vector<std::pair<usize, s32>> pending{{index, -1}}; // the object and its node's parent
    std::vector<bool> visited(objects.size());
    visited[index] = true;
    while (!pending.empty()) {
        const auto [object, parent] = pending.front();
        pending.erase(pending.begin());
        const WorldObject& source = objects[object];
        TreeNodeInfo node;
        node.name = source.name;
        node.object = source.name;
        node.objectFlags = source.objectFlags;
        node.parent = parent;
        node.position = parent < 0 ? Vec3{0.0f, 0.0f, 0.0f} : source.position;
        const auto nodeIndex = static_cast<s32>(tree.nodes.size());
        tree.nodes.push_back(std::move(node));
        for (s32 child = source.child; child >= 0 && static_cast<usize>(child) < objects.size();
             child = objects[static_cast<usize>(child)].next) {
            if (visited[static_cast<usize>(child)]) {
                break;
            }
            visited[static_cast<usize>(child)] = true;
            pending.emplace_back(static_cast<usize>(child), nodeIndex);
        }
    }
    return tree;
}

SpawnedObjects::Source* SpawnedObjects::sourceOf(const std::filesystem::path& unpackedRoot,
                                                 const LevelRef& level) {
    for (const auto& source : m_sources) {
        if (source->level == level.name) {
            return source.get();
        }
    }
    auto source = std::make_unique<Source>();
    source->level = level.name;
    const std::filesystem::path directory = unpackedRoot / level.directory;
    if (!source->layout.load(directory) || !source->models.load(directory) ||
        !source->textures.load(directory)) {
        log::warn("Spawned objects: {} is not unpacked under {}", level.name, directory.string());
        return nullptr;
    }
    if (!source->items.load(unpackedRoot, level)) {
        log::warn("Spawned objects: {} lends no item textures; some may be missing", level.name);
    }
    m_sources.push_back(std::move(source));
    return m_sources.back().get();
}

bool SpawnedObjects::spawn(RenderDevice& device, const std::filesystem::path& unpackedRoot,
                           const LevelRef& level, std::string_view name, const Mat4& transform) {
    if (m_spawned.size() >= kMaxObjects) {
        return false;
    }
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            if (!std::isfinite(transform[column][row])) {
                return false;
            }
        }
    }
    Source* source = sourceOf(unpackedRoot, level);
    if (source == nullptr) {
        return false;
    }
    const auto index = objectNamed(source->layout, name);
    if (!index.has_value()) {
        return false;
    }
    Spawn spawned{level.name, source->layout.objects()[*index].name, transform};
    if (!figureOf(spawned)) {
        auto figure = std::make_unique<Figure>();
        figure->level = spawned.level;
        figure->name = spawned.name;
        figure->tree = treeOf(source->layout, *index);
        const auto lenders = source->items.textureLenders();
        if (!figure->model.bind(figure->tree, source->models, source->textures, device,
                                std::span<TextureSet* const>{lenders})) {
            log::warn("Spawned objects: {} of {} has no drawable body", spawned.name, level.name);
            return false;
        }
        m_figures.push_back(std::move(figure));
    }
    m_spawned.push_back(std::move(spawned));
    return true;
}

const SpawnedObjects::Figure* SpawnedObjects::figureOf(const Spawn& spawned) const {
    const auto found = std::ranges::find_if(m_figures, [&](const auto& figure) {
        return figure->level == spawned.level && figure->name == spawned.name;
    });
    return found == m_figures.end() ? nullptr : found->get();
}

void SpawnedObjects::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                          TreeModel::Pass pass) const {
    for (usize index = 0; index < m_spawned.size(); ++index) {
        const auto& spawned = m_spawned[index];
        const Figure* figure = figureOf(spawned);
        if (!figure) {
            continue;
        }
        figure->model.setHighlight(static_cast<s32>(index) == m_highlightedId ? m_highlight
                                                                              : nullptr);
        figure->model.draw(device, clip, spawned.transform, lighting, {}, nullptr, 1.0f, pass);
        figure->model.setHighlight(nullptr);
    }
}

SpawnedObjects::View SpawnedObjects::view(usize index) const {
    if (index >= m_spawned.size()) {
        return {};
    }
    const Spawn& spawned = m_spawned[index];
    const Figure* figure = figureOf(spawned);
    if (!figure) {
        return {};
    }
    const Vec3 low = figure->model.minBounds();
    const Vec3 high = figure->model.maxBounds();
    const Vec3 centre{spawned.transform * Vec4{(low + high) * 0.5f, 1.0f}};
    return {spawned.name, centre, std::max(glm::distance(low, high) * 0.5f, 1.0f)};
}

void SpawnedObjects::clear() {
    m_spawned.clear();
    m_figures.clear();
    m_sources.clear();
    m_binding = std::make_shared<const u8>(0);
    m_highlightedId = -1;
    m_highlight = nullptr;
}

void SpawnedObjects::saveLocal(LocalSnapshot& saved) const {
    saved.m_spawned = m_spawned;
    saved.m_owner = this;
    saved.m_binding = m_binding;
}

bool SpawnedObjects::canRestoreLocal(const LocalSnapshot& saved) const {
    const auto binding = saved.m_binding.lock();
    return saved.m_owner == this && binding && binding == m_binding &&
           saved.m_spawned.size() <= kMaxObjects &&
           std::ranges::all_of(saved.m_spawned,
                               [this](const Spawn& spawn) { return figureOf(spawn) != nullptr; });
}

bool SpawnedObjects::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    return canRestoreLocal(left) && canRestoreLocal(right) && left.m_spawned == right.m_spawned;
}

bool SpawnedObjects::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_spawned = saved.m_spawned;
    return true;
}

bool SpawnedObjects::appendNativeDigest(NativeStateDigest& digest) const {
    digest.string("SpawnedObjects/1");
    digest.value(m_spawned);
    return digest.good();
}

bool SpawnedObjects::appendLocalDigest(NativeStateDigest& digest,
                                       const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    // Canonical asset recipes are independent of cache order or abandoned births.
    digest.string("SpawnedObjects/1");
    digest.value(saved.m_spawned);
    return digest.good();
}

} // namespace gdl::game
