#include "game/audio/LegendsSounds.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"
#include "engine/io/Sha256.h"
#include "engine/platform/Paths.h"

namespace gdl::game {

bool LegendsSounds::openN64(const std::filesystem::path& root) {
    m_entries.clear();
    try {
        FileStream file(root / "_program/game.bin");
        if (file.size() < 0x7b514 - 0x400 || file.size() > 4 * 1024 * 1024) {
            throw FormatError("unsupported native N64 sound routing program");
        }
        const auto program = file.readExact(static_cast<usize>(file.size()));
        Sha256 hash;
        hash.update(program);
        if (hash.hex() != "0b60632eb50c7e77957a5c28106b4f353ba69dccbe0b37f1f68d37a36fd6e585") {
            throw FormatError("N64 sound routing requires the audited US program");
        }
        // e007a0c0 resolves tokens against 124 twenty-byte rows at e007ab4c.
        // +0 token, +2 replacement, +4 gain, +5 pan, +16 frequency multiplier.
        // e007a25c also handles narration flags separately from MusyX effects.
        const usize count = readU32BE(program, 0x7b510 - 0x400);
        if (count != 124) {
            throw FormatError("unsupported native N64 sound routing count");
        }
        std::vector<Entry> entries;
        entries.reserve(count);
        for (usize i = 0; i < count; ++i) {
            const usize at = 0x7ab4c - 0x400 + i * 20;
            entries.push_back({readU16BE(program, at), readU16BE(program, at + 2), program[at + 4],
                               program[at + 5], std::bit_cast<f32>(readU32BE(program, at + 16))});
        }
        if (!m_effects.open(root / "audio")) {
            return false;
        }
        m_entries = std::move(entries);
        return true;
    } catch (const std::exception& error) {
        log::warn("Legends sound routing {}: {}", paths::utf8(root), error.what());
        return false;
    }
}

std::optional<LegendsSoundCue> LegendsSounds::resolve(u32 token) const {
    if (m_entries.empty() || !m_effects.opened() || token == 0xffff || token == 0xffffffff) {
        return std::nullopt;
    }
    Entry entry;
    if ((token & 0x10000U) != 0) {
        if ((token & ~0x10fffU) != 0 || (token & 0xfffU) >= m_entries.size()) {
            return std::nullopt;
        }
        entry = m_entries[token & 0xfffU];
    } else {
        if (token > 0xffff) {
            return std::nullopt;
        }
        const auto found = std::ranges::find(m_entries, token, &Entry::token);
        if (found != m_entries.end()) {
            entry = *found;
        }
    }
    if (entry.replacement != 0xffff) {
        token = entry.replacement;
    }
    // e007a25c routes 0x1000/0x4000 to streamed narration and 0x2000 to
    // no effect. Do not mask those into unrelated valid MusyX effect IDs.
    if ((token & 0x7000U) != 0 || !std::isfinite(entry.pitch) || entry.pitch <= 0 ||
        entry.pitch > 8) {
        return std::nullopt;
    }
    const auto effect = static_cast<u16>(token & 0x3ffU);
    if (!m_effects.contains(effect)) {
        return std::nullopt;
    }
    // e000d0c8 quantizes game byte controls to MusyX's seven-bit controls.
    const auto volume = static_cast<u32>(static_cast<f32>(entry.volume) / 255.0f * 127.0f);
    const f32 pan = static_cast<f32>(static_cast<s32>(entry.pan) - 128) / 128.0f;
    const auto panByte = static_cast<u32>((pan + 1.0f) * .5f * 127.0f);
    return LegendsSoundCue{effect, static_cast<f32>(volume) / 127.0f,
                           std::clamp((static_cast<f32>(panByte) - 64.0f) / 63.0f, -1.0f, 1.0f),
                           entry.pitch};
}

SoundHandle LegendsSounds::play(SoundPlayer& output, u32 token, f32 volume, bool loop) {
    if (!std::isfinite(volume) || volume <= 0) {
        return kNoSound;
    }
    const auto cue = resolve(token);
    if (!cue) {
        return kNoSound;
    }
    auto source = m_effects.create(cue->effect, cue->pitch, cue->volume, cue->pan);
    if (!source) {
        return kNoSound;
    }
    return output.playStream(std::move(source), loop, std::min(volume, 1.0f),
                             SoundCategory::Effects);
}

} // namespace gdl::game
