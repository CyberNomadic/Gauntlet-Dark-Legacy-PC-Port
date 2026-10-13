#include "game/world/Rubble.h"

#include <string>

#include "engine/assets/AssetScope.h"

namespace gdl::game {

bool Rubble::leave(RenderDevice& device, std::span<ItemArchive* const> archives,
                   std::string_view object, const Mat4& transform) {
    const auto hit = AssetScope(archives).findObject(object);
    if (!hit.has_value()) {
        return false;
    }
    ItemArchive* archive = hit->archive;
    TreeInfo tree;
    tree.name = std::string(object);
    TreeNodeInfo node;
    node.name = std::string(object);
    node.object = hit->name;
    tree.nodes.push_back(node);
    auto piece = std::make_unique<Piece>();
    if (!piece->model.bind(tree, archive->models, archive->textures, device)) {
        return false;
    }
    piece->transform = transform;
    piece->archive = archive;
    piece->object = object;
    m_pieces.push_back(std::move(piece));
    return true;
}

std::vector<Rubble::Presentation> Rubble::presentation() const {
    std::vector<Presentation> result;
    for (usize index = 0; index < m_pieces.size(); ++index) {
        const auto& piece = *m_pieces[index];
        result.push_back({index, piece.archive, piece.object, piece.transform});
    }
    return result;
}

void Rubble::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const {
    for (const std::unique_ptr<Piece>& piece : m_pieces) {
        piece->model.draw(device, clip, piece->transform, lighting);
    }
}

} // namespace gdl::game
