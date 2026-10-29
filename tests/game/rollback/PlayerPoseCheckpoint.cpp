#include "game/rollback/PlayerPoseCheckpoint.h"

#include <bit>
#include <string>
#include <utility>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
void vector(std::vector<u8>& bytes, const Vec3& value) {
    for (s32 axis = 0; axis < 3; ++axis) {
        word(bytes, std::bit_cast<u32>(value[axis]));
    }
}
Vec3 vector(ByteReader& reader) {
    return {std::bit_cast<f32>(reader.readU32()), std::bit_cast<f32>(reader.readU32()),
            std::bit_cast<f32>(reader.readU32())};
}
void pose(std::vector<u8>& bytes, const NodePose& value) {
    vector(bytes, value.rotation);
    vector(bytes, value.position);
    vector(bytes, value.scale);
    word(bytes, static_cast<u32>(value.rotationOrder));
}
std::optional<NodePose> pose(ByteReader& reader) {
    NodePose value;
    value.rotation = vector(reader);
    value.position = vector(reader);
    value.scale = vector(reader);
    const u32 order = reader.readU32();
    if (order > static_cast<u32>(NodePose::RotationOrder::YawPitchRoll)) {
        return std::nullopt;
    }
    value.rotationOrder = static_cast<NodePose::RotationOrder>(order);
    return value;
}
std::string name(ByteReader& reader, usize size) {
    const auto bytes = reader.readBytes(size);
    return {bytes.begin(), bytes.end()};
}
} // namespace

std::optional<std::vector<u8>>
PlayerPoseCheckpoint::encode(const PlayerAnimator::TransitionPose& state) {
    if (!state.valid()) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(kHeaderBytes + state.current.tree.size() +
                  state.current.nodes.size() * (kNodeBytes + 16));
    word(bytes, static_cast<u32>(state.current.tree.size()));
    word(bytes, static_cast<u32>(state.current.nodes.size()));
    bytes.insert(bytes.end(), state.current.tree.begin(), state.current.tree.end());
    for (usize index = 0; index < state.current.nodes.size(); ++index) {
        const auto& node = state.current.nodes[index];
        word(bytes, static_cast<u32>(node.name.size()));
        bytes.insert(bytes.end(), node.name.begin(), node.name.end());
        word(bytes, std::bit_cast<u32>(node.parent));
        vector(bytes, node.rest);
        pose(bytes, state.current.poses[index]);
        pose(bytes, state.previous.poses[index]);
    }
    return bytes;
}

std::optional<PlayerAnimator::TransitionPose>
PlayerPoseCheckpoint::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    const u32 treeLength = reader.readU32();
    const u32 count = reader.readU32();
    if (treeLength > TreePose::Checkpoint::kMaxName || count == 0 ||
        count > TreePose::Checkpoint::kMaxNodes ||
        reader.remaining() < treeLength + static_cast<usize>(count) * kNodeBytes) {
        return std::nullopt;
    }
    PlayerAnimator::TransitionPose state;
    state.current.tree = name(reader, treeLength);
    state.current.nodes.reserve(count);
    state.current.poses.reserve(count);
    state.previous.poses.reserve(count);
    for (u32 index = 0; index < count; ++index) {
        if (reader.remaining() < kNodeBytes) {
            return std::nullopt;
        }
        const u32 length = reader.readU32();
        if (length > TreePose::Checkpoint::kMaxName ||
            reader.remaining() < kNodeBytes - sizeof(u32) + length) {
            return std::nullopt;
        }
        TreePose::Checkpoint::Node node;
        node.name = name(reader, length);
        node.parent = std::bit_cast<s32>(reader.readU32());
        node.rest = vector(reader);
        const auto current = pose(reader);
        const auto previous = pose(reader);
        if (!current || !previous) {
            return std::nullopt;
        }
        state.current.nodes.push_back(std::move(node));
        state.current.poses.push_back(*current);
        state.previous.poses.push_back(*previous);
    }
    state.previous.tree = state.current.tree;
    state.previous.nodes = state.current.nodes;
    return reader.atEnd() && state.valid() ? std::optional{std::move(state)} : std::nullopt;
}
} // namespace gdl::game
