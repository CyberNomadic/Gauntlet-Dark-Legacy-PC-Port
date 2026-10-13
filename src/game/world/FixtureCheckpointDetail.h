#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <tuple>

#include "engine/assets/WorldLayout.h"
#include "engine/world/WorldCollision.h"

namespace gdl::game::fixture_checkpoint {
inline constexpr f32 kMagnitude = 1'000'000;
inline bool scalar(f32 value) {
    return std::isfinite(value) && std::abs(value) <= kMagnitude;
}
inline bool vector(const Vec3& value) {
    return scalar(value.x) && scalar(value.y) && scalar(value.z);
}
inline bool matrix(const Mat4& value) {
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            if (!scalar(value[column][row])) {
                return false;
            }
        }
    }
    return value[0].w == 0 && value[1].w == 0 && value[2].w == 0 && value[3].w == 1;
}
inline bool name(const std::string& value) {
    return value.size() <= 64 && value.find('\0') == std::string::npos;
}
inline auto values(const ItemInfo& value) {
    return std::tie(value.type, value.subtype, value.name, value.radius, value.height, value.xSize,
                    value.zSize, value.collisionType, value.collisionFlags, value.collisionOffset,
                    value.objectFlags, value.properties, value.value, value.armor, value.hitPoints,
                    value.activeType, value.activeOff, value.activeOn, value.choices);
}
inline bool equal(const ItemInstance& a, const ItemInstance& b) {
    if (std::tie(a.info, a.minPlayers, a.flags, a.name, a.position, a.rotation, a.params) !=
            std::tie(b.info, b.minPlayers, b.flags, b.name, b.position, b.rotation, b.params) ||
        a.collision.size() != b.collision.size()) {
        return false;
    }
    for (usize index = 0; index < a.collision.size(); ++index) {
        if (a.collision[index].normal != b.collision[index].normal ||
            a.collision[index].vertices != b.collision[index].vertices) {
            return false;
        }
    }
    return true;
}
inline bool equal(std::span<const CollisionTriangle> a, std::span<const CollisionTriangle> b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
               return std::tie(x.normal, x.vertices, x.object, x.objectFlags) ==
                      std::tie(y.normal, y.vertices, y.object, y.objectFlags);
           });
}
/** Full authored placement, not merely a name which could resolve another item. */
struct Placement {
    usize index = 0;
    ItemInfo info;
    ItemInstance instance;
    bool operator==(const Placement& other) const {
        return index == other.index && values(info) == values(other.info) &&
               equal(instance, other.instance);
    }
    bool valid() const {
        if (index > 65535 || !name(info.name) || !scalar(info.radius) || !scalar(info.height) ||
            !scalar(info.xSize) || !scalar(info.zSize) || !vector(info.collisionOffset) ||
            info.choices.size() > 65535 || instance.info < 0 || instance.info > 65535 ||
            instance.minPlayers < 0 || instance.minPlayers > 255 || !name(instance.name) ||
            !vector(instance.position) || !vector(instance.rotation) ||
            instance.collision.size() > 65535) {
            return false;
        }
        return std::ranges::all_of(instance.collision, [](const auto& triangle) {
            return vector(triangle.normal) && std::ranges::all_of(triangle.vertices, vector);
        });
    }
    bool matches(const WorldLayout& layout) const {
        return index < layout.itemInstances().size() &&
               equal(instance, layout.itemInstances()[index]) && instance.info >= 0 &&
               static_cast<usize>(instance.info) < layout.itemInfos().size() &&
               values(info) == values(layout.itemInfos()[static_cast<usize>(instance.info)]);
    }
};
inline std::optional<Placement> placement(const WorldLayout* layout, usize index) {
    if (!layout || index >= layout->itemInstances().size()) {
        return std::nullopt;
    }
    const auto& instance = layout->itemInstances()[index];
    if (instance.info < 0 || static_cast<usize>(instance.info) >= layout->itemInfos().size()) {
        return std::nullopt;
    }
    return Placement{index, layout->itemInfos()[static_cast<usize>(instance.info)], instance};
}
} // namespace gdl::game::fixture_checkpoint
