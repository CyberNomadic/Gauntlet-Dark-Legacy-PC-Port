#include "game/rollback/InputJournal.h"

#include <algorithm>
#include <bit>
#include <exception>

#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDLJ");
constexpr u8 kLastFlag = 1;

void writeU16(std::vector<u8>& bytes, u16 value) {
    bytes.push_back(static_cast<u8>(value));
    bytes.push_back(static_cast<u8>(value >> 8U));
}
void writeU32(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
void writeU64(std::vector<u8>& bytes, u64 value) {
    for (u32 shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
void writeF32(std::vector<u8>& bytes, f32 value) {
    writeU32(bytes, std::bit_cast<u32>(value));
}
f32 readF32(ByteReader& reader) {
    return std::bit_cast<f32>(reader.readU32());
}
u64 readU64(ByteReader& reader) {
    const u64 low = reader.readU32();
    const u64 high = reader.readU32();
    return low | (high << 32U);
}

bool sameControls(const InputCommand& a, const InputCommand& b) {
    const auto bits = [](f32 value) { return std::bit_cast<u32>(value); };
    const Vec3 aimA = a.aimDirection.value_or(Vec3{0});
    const Vec3 aimB = b.aimDirection.value_or(Vec3{0});
    return a.grant == b.grant && bits(a.direction.x) == bits(b.direction.x) &&
           bits(a.direction.y) == bits(b.direction.y) && bits(a.magnitude) == bits(b.magnitude) &&
           a.aimDirection.has_value() == b.aimDirection.has_value() &&
           bits(aimA.x) == bits(aimB.x) && bits(aimA.y) == bits(aimB.y) &&
           bits(aimA.z) == bits(aimB.z) && a.heldButtons == b.heldButtons &&
           a.pressedButtons == b.pressedButtons && a.hintTopic == b.hintTopic &&
           a.hintRevision == b.hintRevision;
}

void writeRun(std::vector<u8>& bytes, const InputCommand& command, u16 run) {
    writeU32(bytes, command.grant);
    writeF32(bytes, command.direction.x);
    writeF32(bytes, command.direction.y);
    writeF32(bytes, command.magnitude);
    bytes.push_back(command.aimDirection ? 1 : 0);
    const Vec3 aim = command.aimDirection.value_or(Vec3{0});
    writeF32(bytes, aim.x);
    writeF32(bytes, aim.y);
    writeF32(bytes, aim.z);
    writeU32(bytes, command.heldButtons);
    writeU32(bytes, command.pressedButtons);
    bytes.push_back(command.hintTopic);
    bytes.push_back(command.hintRevision);
    writeU16(bytes, run);
}

usize seatCount(u8 seats) {
    return static_cast<usize>(std::popcount(seats));
}

/** The number of runs each present seat's commands pack into. */
usize runCount(std::span<const InputCommand> commands, u16 count) {
    usize runs = 0;
    for (usize start = 0; start + count <= commands.size(); start += count) {
        for (usize i = 0; i < count; ++i) {
            if (i == 0 || !sameControls(commands[start + i], commands[start + i - 1])) {
                ++runs;
            }
        }
    }
    return runs;
}
} // namespace

bool EpochInputJournal::open(u64 epoch, const RollbackInputs::Owners& owners) {
    if (epoch == 0 || std::ranges::all_of(owners, [](u8 owner) { return owner == 0; })) {
        return false;
    }
    m_epoch = epoch;
    m_owners = owners;
    m_frames.clear();
    return true;
}

void EpochInputJournal::clear() {
    m_epoch = 0;
    m_owners = {};
    m_frames.clear();
}

bool EpochInputJournal::append(u64 tick, const JournalFrame& frame) {
    if (!opened() || tick != m_frames.size() + 1 || m_frames.size() >= kMaxFrames) {
        return false;
    }
    for (usize seat = 0; seat < frame.size(); ++seat) {
        const auto& command = frame[seat];
        if (m_owners[seat] == 0) {
            continue;
        }
        if (command.epoch != m_epoch || command.tick != tick || command.seat != seat ||
            !command.valid()) {
            return false;
        }
    }
    m_frames.push_back(frame);
    return true;
}

bool JournalPacket::Chunk::valid() const {
    if (epoch == 0 || firstTick == 0 || count == 0 || count > kMaxTicks || seats == 0 ||
        (seats & ~((1U << InputCommand::kSeats) - 1U)) != 0 ||
        commands.size() != seatCount(seats) * count) {
        return false;
    }
    usize index = 0;
    for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
        if ((seats & (1U << seat)) == 0) {
            continue;
        }
        for (u16 i = 0; i < count; ++i, ++index) {
            const auto& command = commands[index];
            if (command.epoch != epoch || command.tick != firstTick + i || command.seat != seat ||
                !command.valid()) {
                return false;
            }
        }
    }
    return true;
}

bool JournalPacket::recognizes(std::span<const u8> bytes) {
    return bytes.size() >= 4 && ByteReader(bytes).readU32() == kMagic;
}

std::optional<std::vector<u8>> JournalPacket::encode(const Chunk& chunk) {
    if (!chunk.valid()) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(kHeaderBytes + runCount(chunk.commands, chunk.count) * kRunBytes);
    writeU32(bytes, kMagic);
    bytes.push_back(kVersion);
    bytes.push_back(chunk.last ? kLastFlag : 0);
    bytes.push_back(chunk.seats);
    bytes.push_back(0);
    writeU64(bytes, chunk.epoch);
    writeU64(bytes, chunk.firstTick);
    writeU16(bytes, chunk.count);
    writeU16(bytes, 0);
    for (usize start = 0; start < chunk.commands.size(); start += chunk.count) {
        usize i = 0;
        while (i < chunk.count) {
            usize run = 1;
            while (i + run < chunk.count &&
                   sameControls(chunk.commands[start + i + run], chunk.commands[start + i])) {
                ++run;
            }
            writeRun(bytes, chunk.commands[start + i], static_cast<u16>(run));
            i += run;
        }
    }
    if (bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    return bytes;
}

std::optional<JournalPacket::Chunk> JournalPacket::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes || !recognizes(bytes)) {
        return std::nullopt;
    }
    try {
        ByteReader reader(bytes);
        reader.skip(4);
        if (reader.readU8() != kVersion) {
            return std::nullopt;
        }
        const u8 flags = reader.readU8();
        Chunk chunk;
        chunk.seats = reader.readU8();
        if (reader.readU8() != 0 || (flags & ~kLastFlag) != 0) {
            return std::nullopt;
        }
        chunk.last = (flags & kLastFlag) != 0;
        chunk.epoch = readU64(reader);
        chunk.firstTick = readU64(reader);
        chunk.count = reader.readU16();
        if (reader.readU16() != 0 || chunk.count == 0 || chunk.count > kMaxTicks ||
            chunk.seats == 0) {
            return std::nullopt;
        }
        chunk.commands.reserve(seatCount(chunk.seats) * chunk.count);
        for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
            if ((chunk.seats & (1U << seat)) == 0) {
                continue;
            }
            u16 covered = 0;
            while (covered < chunk.count) {
                InputCommand command;
                command.epoch = chunk.epoch;
                command.seat = seat;
                command.grant = reader.readU32();
                command.direction.x = readF32(reader);
                command.direction.y = readF32(reader);
                command.magnitude = readF32(reader);
                const u8 aimPresent = reader.readU8();
                Vec3 aim;
                aim.x = readF32(reader);
                aim.y = readF32(reader);
                aim.z = readF32(reader);
                if (aimPresent > 1 || (aimPresent == 0 && aim != Vec3{0})) {
                    return std::nullopt;
                }
                if (aimPresent != 0) {
                    command.aimDirection = aim;
                }
                command.heldButtons = reader.readU32();
                command.pressedButtons = reader.readU32();
                command.hintTopic = reader.readU8();
                command.hintRevision = reader.readU8();
                const u16 run = reader.readU16();
                if (run == 0 || covered + run > chunk.count) {
                    return std::nullopt;
                }
                for (u16 i = 0; i < run; ++i) {
                    command.tick = chunk.firstTick + covered + i;
                    chunk.commands.push_back(command);
                }
                covered = static_cast<u16>(covered + run);
            }
        }
        if (!reader.atEnd() || !chunk.valid()) {
            return std::nullopt;
        }
        return chunk;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<JournalPacket::Chunk> JournalPacket::chunks(const EpochInputJournal& journal,
                                                        u64 firstTick, u64 lastTick,
                                                        bool markLast) {
    std::vector<Chunk> result;
    if (!journal.opened() || firstTick == 0 || lastTick < firstTick ||
        lastTick > journal.through()) {
        return result;
    }
    u8 seats = 0;
    for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
        if (journal.owners()[seat] != 0) {
            seats |= static_cast<u8>(1U << seat);
        }
    }
    const usize present = seatCount(seats);
    u64 start = firstTick;
    while (start <= lastTick) {
        // Grow the chunk a tick at a time; a tick costs a run only where a seat changed.
        usize bytes = kHeaderBytes + present * kRunBytes;
        u64 end = start;
        while (end < lastTick && end - start + 1 < kMaxTicks) {
            usize added = 0;
            const JournalFrame& previous = *journal.frame(end);
            const JournalFrame& next = *journal.frame(end + 1);
            for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
                if ((seats & (1U << seat)) != 0 && !sameControls(next[seat], previous[seat])) {
                    added += kRunBytes;
                }
            }
            if (bytes + added > kMaxBytes) {
                break;
            }
            bytes += added;
            ++end;
        }
        Chunk chunk;
        chunk.epoch = journal.epoch();
        chunk.firstTick = start;
        chunk.count = static_cast<u16>(end - start + 1);
        chunk.seats = seats;
        chunk.last = markLast && end == lastTick;
        for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
            if ((seats & (1U << seat)) == 0) {
                continue;
            }
            for (u64 tick = start; tick <= end; ++tick) {
                chunk.commands.push_back((*journal.frame(tick))[seat]);
            }
        }
        result.push_back(std::move(chunk));
        start = end + 1;
    }
    return result;
}

std::vector<JournalPacket::Chunk>
JournalPacket::tailChunks(u64 epoch, std::vector<InputCommand> commands, bool markLast) {
    std::vector<Chunk> result;
    std::erase_if(commands, [epoch](const InputCommand& command) {
        return command.epoch != epoch || !command.valid() || command.tick == 0;
    });
    std::ranges::sort(commands, [](const InputCommand& a, const InputCommand& b) {
        return a.seat != b.seat ? a.seat < b.seat : a.tick < b.tick;
    });
    // Every tick may be its own run, so this many always fit the wire.
    const usize perChunk = (kMaxBytes - kHeaderBytes) / kRunBytes;
    usize start = 0;
    while (start < commands.size()) {
        usize end = start + 1;
        while (end < commands.size() && end - start < perChunk &&
               commands[end].seat == commands[start].seat &&
               commands[end].tick == commands[end - 1].tick + 1) {
            ++end;
        }
        Chunk chunk;
        chunk.epoch = epoch;
        chunk.firstTick = commands[start].tick;
        chunk.count = static_cast<u16>(end - start);
        chunk.seats = static_cast<u8>(1U << commands[start].seat);
        chunk.commands.assign(commands.begin() + static_cast<std::ptrdiff_t>(start),
                              commands.begin() + static_cast<std::ptrdiff_t>(end));
        result.push_back(std::move(chunk));
        start = end;
    }
    if (markLast && !result.empty()) {
        result.back().last = true;
    }
    return result;
}

} // namespace gdl::game
