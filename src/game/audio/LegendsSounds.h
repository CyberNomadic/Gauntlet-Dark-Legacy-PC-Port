#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "engine/audio/N64LegendsEffects.h"
#include "engine/audio/SoundPlayer.h"

namespace gdl::game {

struct LegendsSoundCue {
    u16 effect = 0;
    f32 volume = 1;
    f32 pan = 0;
    f32 pitch = 1;
};

/** Native Legends cue tokens and game-side defaults, above the MusyX bank.
 * Streamed speech uses a different native codec/path and is not reinterpreted
 * as an effect. This facade never substitutes Dark Legacy sound names. */
class LegendsSounds {
public:
    bool openN64(const std::filesystem::path& nativeRoot);
    bool opened() const { return !m_entries.empty() && m_effects.opened(); }
    std::optional<LegendsSoundCue> resolve(u32 token) const;
    SoundHandle play(SoundPlayer& output, u32 token, f32 volume = 1, bool loop = false);

private:
    struct Entry {
        u16 token = 0xffff;
        u16 replacement = 0xffff;
        u8 volume = 255;
        u8 pan = 128;
        f32 pitch = 1;
    };
    N64LegendsEffects m_effects;
    std::vector<Entry> m_entries;
};

} // namespace gdl::game
