#include "game/level/SpawnedObjects.h"

#include <algorithm>
#include <cctype>
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
            pending.emplace_back(static_cast<usize>(child), nodeIndex);
            if (pending.size() > objects.size()) {
                break; // a cycle in the links; nothing sane follows
            }
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
    Source* source = sourceOf(unpackedRoot, level);
    if (source == nullptr) {
        return false;
    }
    const auto index = objectNamed(source->layout, name);
    if (!index.has_value()) {
        return false;
    }
    auto spawned = std::make_unique<Spawned>();
    spawned->name = source->layout.objects()[*index].name;
    spawned->tree = treeOf(source->layout, *index);
    spawned->transform = transform;
    const auto lenders = source->items.textureLenders();
    if (!spawned->model.bind(spawned->tree, source->models, source->textures, device,
                             std::span<TextureSet* const>{lenders})) {
        log::warn("Spawned objects: {} of {} has no drawable body", spawned->name, level.name);
        return false;
    }
    m_spawned.push_back(std::move(spawned));
    return true;
}

void SpawnedObjects::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                          TreeModel::Pass pass) const {
    for (const auto& spawned : m_spawned) {
        spawned->model.draw(device, clip, spawned->transform, lighting, {}, nullptr, 1.0f, pass);
    }
}

void SpawnedObjects::clear() {
    m_spawned.clear();
    m_sources.clear();
}

} // namespace gdl::game
