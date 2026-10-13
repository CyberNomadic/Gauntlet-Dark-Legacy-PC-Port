#include "game/netplay/PlatformCheckpoint.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDLP");
constexpr u32 kMoving = 0x1000;
void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
void real(std::vector<u8>& bytes, f32 value) {
    word(bytes, std::bit_cast<u32>(value));
}
f32 real(ByteReader& reader) {
    return std::bit_cast<f32>(reader.readU32());
}
} // namespace

bool PlayerFloorState::valid() const {
    if (object < -1 || object > 1'000'000 || (object == -1 && (flags != 0 || placement)) ||
        (placement && (flags & kMoving) == 0)) {
        return false;
    }
    if (!placement) {
        return true;
    }
    const auto& matrix = *placement;
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            const f32 bound = column == 3 ? 1'000'000.0f : 1024.0f;
            if (!std::isfinite(matrix[column][row]) || std::abs(matrix[column][row]) > bound) {
                return false;
            }
        }
    }
    const f32 determinant = glm::determinant(Mat3{matrix});
    return matrix[0].w == 0 && matrix[1].w == 0 && matrix[2].w == 0 && matrix[3].w == 1 &&
           std::isfinite(determinant) && std::abs(determinant) >= 0.000001f;
}

bool PlatformState::valid() const {
    if (ticks > kMaxTicks || (!motion && ticks != 0) || (motion && !motion->valid())) {
        return false;
    }
    return std::ranges::all_of(floors, [&](const auto& floor) { return !floor || floor->valid(); });
}

std::optional<std::vector<u8>> PlatformCheckpoint::encode(const PlatformState& state) {
    if (!state.valid()) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    u32 mask = 0;
    for (usize seat = 0; seat < state.floors.size(); ++seat) {
        if (state.floors[seat]) {
            mask |= 1U << seat;
        }
    }
    word(bytes, kMagic);
    word(bytes, 1);
    word(bytes, state.ticks);
    word(bytes, mask);
    word(bytes, state.motion ? 1U | (state.motion->pauseLoops ? 2U : 0U) : 0U);
    word(bytes, state.motion ? static_cast<u32>(state.motion->animations.size()) : 0U);
    word(bytes, state.motion ? static_cast<u32>(state.motion->triggers.targets.size()) : 0U);
    word(bytes, state.motion ? static_cast<u32>(state.motion->rotators.size()) : 0U);
    word(bytes, state.motion ? static_cast<u32>(state.motion->collision.size()) : 0U);
    real(bytes, state.motion ? state.motion->triggers.frameRemainder : 0);
    for (const auto& floor : state.floors) {
        if (!floor) {
            continue;
        }
        word(bytes, std::bit_cast<u32>(floor->object));
        word(bytes, floor->flags);
        word(bytes, floor->placement ? 1U : 0U);
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 3; ++row) {
                real(bytes, floor->placement ? (*floor->placement)[column][row] : 0);
            }
        }
    }
    if (state.motion) {
        for (const auto& cursor : state.motion->animations) {
            word(bytes, static_cast<u32>(cursor.object));
            word(bytes, static_cast<u32>(cursor.frames));
            real(bytes, cursor.frame);
            word(bytes, (cursor.reverse ? 1U : 0U) | (cursor.once ? 2U : 0U) |
                            (cursor.finished ? 4U : 0U) | (cursor.held ? 8U : 0U) |
                            (cursor.presentationCut ? 16U : 0U));
        }
        for (const auto& target : state.motion->triggers.targets) {
            word(bytes, static_cast<u32>(target.object));
            word(bytes, (target.open ? 1U : 0U) | (target.settled ? 2U : 0U) |
                            (target.pressed ? 4U : 0U) | (target.wholePartyReady ? 8U : 0U) |
                            (target.returning ? 16U : 0U) | (target.forced ? 32U : 0U));
            real(bytes, target.alpha);
            real(bytes, target.height);
        }
        for (const auto& cursor : state.motion->rotators) {
            word(bytes, static_cast<u32>(cursor.instance));
            word(bytes, static_cast<u32>(cursor.object));
            real(bytes, cursor.turned);
            word(bytes, (cursor.started ? 1U : 0U) | (cursor.done ? 2U : 0U));
        }
        bytes.insert(bytes.end(), state.motion->collision.begin(), state.motion->collision.end());
    }
    return bytes;
}

std::optional<PlatformState> PlatformCheckpoint::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    if (reader.readU32() != kMagic || reader.readU32() != 1) {
        return std::nullopt;
    }
    PlatformState result;
    result.ticks = reader.readU32();
    const u32 mask = reader.readU32();
    const u32 flags = reader.readU32();
    const usize animations = reader.readU32();
    const usize targets = reader.readU32();
    const usize rotators = reader.readU32();
    const usize objects = reader.readU32();
    const f32 remainder = real(reader);
    if (mask >= 16 || (flags & ~3U) != 0 || animations > WorldMotion::kMaxAnimations ||
        targets > LevelTriggers::Motion::kMaxTargets || rotators > Rotators::kMaxCursors ||
        objects > WorldMotion::kMaxObjects ||
        bytes.size() != kHeaderBytes + std::popcount(mask) * kFloorBytes +
                            (animations + targets + rotators) * kCursorBytes + objects ||
        ((flags & 1U) == 0 && (flags != 0 || animations != 0 || targets != 0 || rotators != 0 ||
                               objects != 0 || std::bit_cast<u32>(remainder) != 0))) {
        return std::nullopt;
    }
    for (usize seat = 0; seat < result.floors.size(); ++seat) {
        if ((mask & (1U << seat)) == 0) {
            continue;
        }
        PlayerFloorState floor;
        floor.object = std::bit_cast<s32>(reader.readU32());
        floor.flags = reader.readU32();
        const u32 placed = reader.readU32();
        if (placed > 1) {
            return std::nullopt;
        }
        Mat4 matrix{1};
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 3; ++row) {
                const u32 bits = reader.readU32();
                if (placed == 0 && bits != 0) {
                    return std::nullopt;
                }
                matrix[column][row] = std::bit_cast<f32>(bits);
            }
        }
        if (placed != 0) {
            floor.placement = matrix;
        }
        result.floors[seat] = floor;
    }
    if ((flags & 1U) != 0) {
        auto& motion = result.motion.emplace();
        motion.pauseLoops = (flags & 2U) != 0;
        motion.triggers.frameRemainder = remainder;
        for (usize index = 0; index < animations; ++index) {
            WorldAnimator::Cursor cursor;
            cursor.object = std::bit_cast<s32>(reader.readU32());
            cursor.frames = std::bit_cast<s32>(reader.readU32());
            cursor.frame = real(reader);
            const u32 bits = reader.readU32();
            if ((bits & ~31U) != 0) {
                return std::nullopt;
            }
            cursor.reverse = (bits & 1U) != 0;
            cursor.once = (bits & 2U) != 0;
            cursor.finished = (bits & 4U) != 0;
            cursor.held = (bits & 8U) != 0;
            cursor.presentationCut = (bits & 16U) != 0;
            motion.animations.push_back(cursor);
        }
        for (usize index = 0; index < targets; ++index) {
            LevelTriggers::Motion::Target target;
            target.object = std::bit_cast<s32>(reader.readU32());
            const u32 bits = reader.readU32();
            if ((bits & ~63U) != 0) {
                return std::nullopt;
            }
            target.open = (bits & 1U) != 0;
            target.settled = (bits & 2U) != 0;
            target.pressed = (bits & 4U) != 0;
            target.wholePartyReady = (bits & 8U) != 0;
            target.returning = (bits & 16U) != 0;
            target.forced = (bits & 32U) != 0;
            target.alpha = real(reader);
            target.height = real(reader);
            motion.triggers.targets.push_back(target);
        }
        for (usize index = 0; index < rotators; ++index) {
            Rotators::Cursor cursor;
            cursor.instance = std::bit_cast<s32>(reader.readU32());
            cursor.object = std::bit_cast<s32>(reader.readU32());
            cursor.turned = real(reader);
            const u32 bits = reader.readU32();
            if ((bits & ~3U) != 0) {
                return std::nullopt;
            }
            cursor.started = (bits & 1U) != 0;
            cursor.done = (bits & 2U) != 0;
            motion.rotators.push_back(cursor);
        }
        const auto collision = reader.readBytes(objects);
        motion.collision.assign(collision.begin(), collision.end());
    }
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}
} // namespace gdl::game
