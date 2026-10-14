#include "game/world/LevelTransporters.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

#include "engine/core/Log.h"

#include "game/world/ItemFigure.h"

namespace gdl::game {
namespace {
s32 parameter(const ItemInstance& instance, usize offset) {
    u32 value = 0;
    for (usize byte = 0; byte < sizeof(value); ++byte) {
        value |= static_cast<u32>(instance.params[offset + byte]) << (byte * 8);
    }
    return std::bit_cast<s32>(value);
}
} // namespace

std::vector<LevelTransporters::Contact> LevelTransporters::contacts(const WorldLayout& layout,
                                                                    s32 players) {
    std::vector<Contact> result;
    const auto& infos = layout.itemInfos();
    const auto& instances = layout.itemInstances();
    for (usize i = 0; i < instances.size(); ++i) {
        const auto& instance = instances[i];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size() ||
            !shownToParty(instance.minPlayers, players)) {
            continue;
        }
        const auto& info = infos[static_cast<usize>(instance.info)];
        if (info.type != kItemType) {
            continue;
        }
        Contact pad;
        pad.instance = static_cast<s32>(i);
        pad.id = parameter(instance, 0);
        pad.destinationId = parameter(instance, 4);
        pad.position = instance.position;
        pad.radius = info.radius;
        pad.height = info.height;
        result.push_back(pad);
    }
    for (usize i = 0; i < result.size(); ++i) {
        for (usize j = 0; j < result.size(); ++j) {
            if (i != j && result[j].id == result[i].destinationId) {
                result[i].destination = j;
                break;
            }
        }
    }
    return result;
}

void LevelTransporters::bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
                             s32 players, ItemArchive* realmItems) {
    clear();
    ItemArchive& art = itemArchiveForTree(items, "TRANS", realmItems);
    const auto tree = art.trees.find("TRANS");
    if (tree) {
        m_tree = &art.trees.tree(*tree);
        m_textures.bind(art.trees.textureAnimations(), art.textures, device);
    }
    for (const auto& contact : contacts(layout, players)) {
        Pad pad;
        static_cast<Contact&>(pad) = contact;
        const auto& instance = layout.itemInstances()[static_cast<usize>(pad.instance)];
        // TRANS includes a floor-level textured skirt. Keep the ordinary fixture
        // clearance on its artwork, without raising the contact or landing anchor.
        pad.transform =
            itemPlacement(pad.position + Vec3{0, ItemFigure::kFloorLift, 0}, instance.rotation);
        if (m_tree != nullptr && pad.model.bind(*m_tree, art.models, art.textures, device)) {
            pad.pose.rest(*m_tree);
            if (const auto sequence = m_tree->findSequence("ACTIVE")) {
                pad.animation.start(m_tree->sequences[*sequence], *sequence);
            }
        }
        m_pads.push_back(std::move(pad));
    }
    for (const auto& pad : m_pads) {
        if (!pad.destination) {
            log::warn("Transporter {}: missing destination {}", pad.id, pad.destinationId);
        }
    }
    animate(0.0f);
}

void LevelTransporters::clear() {
    m_pads.clear();
    m_tree = nullptr;
    m_textures.clear();
    m_presentationAdvanced = false;
}

void LevelTransporters::animate(f32 seconds) {
    m_textures.advance(seconds);
    m_presentationAdvanced = seconds > 0;
    for (auto& pad : m_pads) {
        if (m_tree != nullptr && pad.animation.playing()) {
            pad.previousFrame = pad.animation.presentationFrame();
            pad.previousGeneration = pad.animation.generation();
            pad.animation.advance(seconds, true);
            const auto sequence = pad.animation.sequence();
            const auto frame = static_cast<s32>(pad.animation.frame());
            pad.pose.evaluate(*m_tree, sequence, pad.animation.frame());
            pad.model.setFrame(sequence, frame);
            m_textures.apply(pad.model, *m_tree, sequence, frame);
        }
    }
}

void LevelTransporters::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                             f32 renderAlpha) const {
    for (const auto& pad : m_pads) {
        if (pad.model.bound() && m_tree != nullptr) {
            const f32 blend = renderAlpha < 0 || m_presentationAdvanced ? renderAlpha : 1.0f;
            f32 frame = pad.animation.frame();
            TreePose visualPose;
            const TreePose* pose = &pad.pose;
            if (blend >= 0 && pad.animation.playing()) {
                frame = pad.previousGeneration == pad.animation.generation()
                            ? std::lerp(pad.previousFrame, pad.animation.presentationFrame(),
                                        std::clamp(blend, 0.0f, 1.0f))
                            : pad.animation.presentationFrame();
                visualPose.evaluate(*m_tree, pad.animation.sequence(), frame, false, true);
                pose = &visualPose;
            }
            pad.model.setPresentationFrame(pad.animation.sequence(), frame);
            m_textures.apply(pad.model, *m_tree, pad.animation.sequence(), frame,
                             m_textures.presentationOffset(blend));
            pad.model.draw(device, clip, pad.transform, lighting, pose->matrices());
        }
    }
}

std::optional<usize> LevelTransporters::contact(const Vec3& feet, f32 radius, f32 height) const {
    for (usize i = 0; i < m_pads.size(); ++i) {
        const auto& pad = m_pads[i];
        if (pad.touches(feet, radius, height)) {
            return i;
        }
    }
    return std::nullopt;
}

bool LevelTransporters::Contact::touches(const Vec3& feet, f32 bodyRadius, f32 bodyHeight) const {
    const Vec3 away = feet - position;
    const f32 reach = bodyRadius + radius;
    return away.x * away.x + away.z * away.z < reach * reach && feet.y <= position.y + height &&
           feet.y + bodyHeight >= position.y;
}

bool LevelTransporters::visible(const Vec3& position, f32 radius, const WorldCamera& camera,
                                const CameraView& view) {
    const Vec3 relative = position - camera.position;
    const f32 z = glm::dot(relative, camera.forward());
    const f32 horizontal = view.horizontalFov * 0.5f;
    const f32 vertical = std::atan(std::tan(horizontal) / view.aspect);
    // The entire destination sphere must fit, not just its centre.
    return z >= WorldCamera::kNear + radius && z <= WorldCamera::kFar - radius &&
           std::abs(glm::dot(relative, camera.right())) * std::cos(horizontal) <=
               z * std::sin(horizontal) - radius &&
           std::abs(glm::dot(relative, camera.up())) * std::cos(vertical) <=
               z * std::sin(vertical) - radius;
}

std::optional<Vec3> LevelTransporters::landing(usize source, f32 radius,
                                               const WorldCollision& collision,
                                               const WorldCamera& camera,
                                               const CameraView& view) const {
    if (source >= m_pads.size()) {
        return std::nullopt;
    }
    const auto destinationIndex = m_pads[source].destination;
    if (!destinationIndex.has_value()) {
        return std::nullopt;
    }
    Vec3 destination = m_pads[*destinationIndex].position;
    if (!visible(destination, radius, camera, view)) {
        return std::nullopt;
    }
    const auto floor = collision.floorAt(destination, 4.0f, 10.0f);
    if (!floor) {
        return std::nullopt;
    }
    destination.y = floor->y;
    return destination;
}

std::string_view LevelTransporters::soundForRealm(s32 realm) {
    static constexpr std::array<std::string_view, 13> kSounds{"",
                                                              "S_TRANSPORTA",
                                                              "",
                                                              "S_TRANSPORTC",
                                                              "",
                                                              "",
                                                              "",
                                                              "S_TRANSPORTG",
                                                              "S_TRANSPORTH",
                                                              "S_TRANSPORTI",
                                                              "S_TRANSPORTJ",
                                                              "S_TRANSPORTK",
                                                              "S_TRANSPORTS3"};
    return realm >= 0 && static_cast<usize>(realm) < kSounds.size()
               ? kSounds[static_cast<usize>(realm)]
               : "";
}

} // namespace gdl::game
