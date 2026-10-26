#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/rollback/ConsoleAction.h"

namespace gdl::game {

// Wire values: append new actions only with a protocol version change. Device IDs,
// key bindings, pointers, saves and presentation state never travel in this packet.
enum class CommandHeld : u16 {
    Attack = 1U << 0U,
    ShieldPotion = 1U << 1U,
    Strafe = 1U << 2U,
    StrongAttack = 1U << 3U,
    Turbo = 1U << 4U,
    Combo = 1U << 5U,
    MenuUp = 1U << 6U,
    MenuDown = 1U << 7U,
    MenuLeft = 1U << 8U,
    MenuRight = 1U << 9U,
    ThrowPotion = 1U << 10U, ///< held wind-up strength, including concealed input frames
    FreeMove = 1U << 11U,    ///< Robotron: the keys walk the camera's way, the mouse aims attacks
};

enum class CommandPress : u32 {
    UsePotion = 1U << 0U,
    // Bit 1 was ThrowPotion in v2. It is reserved: a held throw is not a press.
    Defend = 1U << 2U,
    Charge = 1U << 3U,
    Attack = 1U << 4U,
    TurboAttack = 1U << 5U,
    SelectorUp = 1U << 6U,
    SelectorDown = 1U << 7U,
    SelectorLeft = 1U << 8U,
    SelectorRight = 1U << 9U,
    MenuUp = 1U << 10U,
    MenuDown = 1U << 11U,
    MenuLeft = 1U << 12U,
    MenuRight = 1U << 13U,
    MenuSelect = 1U << 14U,
    MenuBack = 1U << 15U,
    MenuStart = 1U << 16U,
    MenuEscape = 1U << 17U,
    MenuAny = 1U << 18U,
};

/** One seat's logical controls at an authoritative simulation tick, not a rendered frame.
 * Epochs separate level/pause contexts; grants separate occupants of the same seat.
 * aimDirection is horizontal world facing, never an authoritative position or hit target.
 * Zero retains current facing; absence selects ordinary camera-relative stick movement. */
struct InputCommand {
    static constexpr usize kSeats = 4;
    static constexpr u32 kHeldMask = (1U << 12U) - 1U;
    static constexpr u32 kPressMask = ((1U << 19U) - 1U) & ~(1U << 1U);
    u64 epoch = 0;
    u64 tick = 0;
    u32 grant = 0;
    u8 seat = 0;
    Vec2 direction{0};
    f32 magnitude = 0;
    std::optional<Vec3> aimDirection;
    u32 heldButtons = 0;
    u32 pressedButtons = 0;
    u8 hintTopic = 0;                     ///< one-shot Sumner topic row + 1; zero means absent
    u8 hintRevision = 0;                  ///< observed menu revision; zero when no topic is present
    std::optional<ConsoleAction> console; ///< host-admitted, never predicted or repeated
    Vec3 consoleFlight{0}; ///< held world-space noclip wish; ignored unless noclip is admitted

    bool held(CommandHeld button) const { return (heldButtons & static_cast<u32>(button)) != 0; }
    bool pressed(CommandPress button) const {
        return (pressedButtons & static_cast<u32>(button)) != 0;
    }
    bool valid() const;

    /** The analogue fields rounded to coarse steps (a 1/256 turn of movement, 1/64 of its
     * magnitude, a 1/256 turn of aim) so that a steady hand and a cursor drifting across a
     * moving character send bit-identical commands tick after tick, which is what the
     * held-input prediction needs to come true. Buttons and identity are untouched. */
    InputCommand steadied() const;
};

/** Versioned, little-endian, bounded input datagrams. No native struct serialization.
 * Four seats can resend four ordinary ticks each in one 968-byte packet. Console
 * mutations consume variable space; callers split only between complete frames. Redundancy recovers
 * short losses without replaying edges; it is not reliable delivery for menu transactions.
 * The transport must authenticate the sender separately; a packet cannot name its peer. */
class InputPacket {
public:
    static constexpr u16 kVersion = 6;
    static constexpr usize kMaxCommands = 16;
    static constexpr usize kHeaderBytes = 8;
    static constexpr usize kCommandBytes = 60; ///< ordinary command; console text is variable
    static constexpr usize kMaxBytes = kHeaderBytes + kMaxCommands * kCommandBytes;

    static usize commandBytes(const InputCommand& command) {
        return kCommandBytes + (command.console ? 1 + command.console->line.size() : 0);
    }

    static std::optional<std::vector<u8>> encode(std::span<const InputCommand> commands);
    static std::optional<std::vector<InputCommand>> decode(std::span<const u8> bytes);
};

/** Outbound redundancy for one peer's locally owned seats. Record once per sampled
 * tick, then send commands(). Keeps four ticks, not four renders or four packets. */
class InputHistory {
public:
    static constexpr u64 kTicks = 4;
    bool record(std::span<const InputCommand> frame);
    std::span<const InputCommand> commands() const { return m_commands; }
    void clear() { m_commands.clear(); }

private:
    std::vector<InputCommand> m_commands;
};

} // namespace gdl::game
