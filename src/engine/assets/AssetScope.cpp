#include "engine/assets/AssetScope.h"

#include <algorithm>

namespace gdl {

void AssetScope::add(ItemArchive& archive) {
    if (!archive.loaded() || std::ranges::find(m_archives, &archive) != m_archives.end()) {
        return;
    }
    m_archives.push_back(&archive);
}

std::optional<TreeHit> AssetScope::findTree(std::string_view name) const {
    for (ItemArchive* archive : m_archives) {
        if (const auto index = archive->trees.find(name)) {
            return TreeHit{archive, *index};
        }
    }
    return std::nullopt;
}

std::optional<ObjectHit> AssetScope::findObject(std::string_view name) const {
    for (const std::string_view suffix : kObjectSuffixes) {
        const std::string spelling = std::string{name} + std::string{suffix};
        for (ItemArchive* archive : m_archives) {
            if (const auto index = archive->models.find(spelling)) {
                return ObjectHit{archive, *index, archive->models.entry(*index).name};
            }
        }
    }
    return std::nullopt;
}

ItemArchive* AssetScope::archiveForTree(std::string_view name) const {
    if (const auto hit = findTree(name)) {
        return hit->archive;
    }
    return primary();
}

std::vector<TextureSet*> AssetScope::textureLenders() const {
    std::vector<TextureSet*> lenders;
    lenders.reserve(m_archives.size());
    for (ItemArchive* archive : m_archives) {
        lenders.push_back(&archive->textures);
    }
    return lenders;
}

} // namespace gdl
