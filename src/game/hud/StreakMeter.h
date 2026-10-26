#pragma once

#include <array>
#include <span>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl {
class Canvas;
class StringTable;
class TextPainter;
class Texture;
} // namespace gdl

namespace gdl::game {

/** What the meter watches of one seat's run each frame: the simulation's count and quiet
 * clock, and the character's record so far. */
struct StreakView {
    s32 player = 0;
    s32 count = 0;
    f32 quiet = 0;
    bool running = false;
    s32 record = 0;
};

/**
 * A style meter in the upper left for every local seat on a run of close blows: the run's
 * letter grade, D to S, beside a bar that rises with the blows and drains through the quiet
 * that ends the run, the count over it and the character's best under it. It shakes and
 * bleeds the higher the grade, flashes on a new grade, celebrates a new record and slams
 * away once the run is over and the narrator has his say. Presentation only: never
 * simulation state, so it is neither saved nor rewound.
 */
class StreakMeter {
public:
    static constexpr s32 kGrades = 5;
    /** Blows that reach each grade: D, C, B, A (the heroic line) and S (the bravery line). */
    static constexpr std::array<s32, kGrades> kGradeCounts{1, 10, 20, 30, 45};
    static constexpr std::string_view kLetters = "DCBAS";
    static constexpr std::array<Color, kGrades> kGradeColors{
        Color::rgba(150, 160, 190), Color::rgba(90, 200, 110), Color::rgba(80, 150, 255),
        Color::rgba(200, 90, 255), Color::rgba(255, 200, 40)};
    static constexpr Color kBlood = Color::rgba(150, 10, 10);
    static constexpr Color kRecord = Color::rgba(255, 230, 90);

    static constexpr s32 kX = 12;         ///< the meter's left, in virtual pixels
    static constexpr s32 kY = 10;         ///< its top
    static constexpr s32 kRowHeight = 58; ///< one local seat's meter under another's
    static constexpr s32 kLetterGap = 10; ///< from the letter's right to the bar
    static constexpr s32 kBarWidth = 108;
    static constexpr s32 kBarHeight = 7;
    static constexpr f32 kLabelScale = 0.5f;
    static constexpr f32 kBestScale = 0.4f;
    static constexpr f32 kRecordScale = 0.75f;

    static constexpr f32 kAppearRate = 6.0f;  ///< presence per second, coming in
    static constexpr f32 kFadeRate = 2.0f;    ///< and going out
    static constexpr f32 kLinger = 0.9f;      ///< seconds the verdict holds before fading
    static constexpr f32 kPunchDecay = 5.0f;  ///< a blow's scale bump, per second
    static constexpr f32 kRankFlash = 1.0f;   ///< seconds a new grade flashes and bursts
    static constexpr f32 kRecordShow = 2.6f;  ///< seconds the record announcement plays
    static constexpr f32 kShakeDecay = 5.0f;  ///< a burst's shake, per second
    static constexpr f32 kMaxFrame = 0.1f;    ///< the longest step one frame may take

    /** The grade a count has reached: -1 before the first blow, else into `kLetters`. */
    static s32 gradeOf(s32 count);
    /** How far a count has climbed through its grade toward the next, 0 to 1; S is full. */
    static f32 bandProgress(s32 count);
    /** The bar's share: the climb through the grade, drained as the quiet runs out. */
    static f32 fillOf(s32 count, f32 quiet);
    /** How hard a grade shakes, in pixels of jitter. */
    static f32 shakeOf(s32 grade);
    /** The letter's scale at a grade: bigger the better. */
    static f32 letterScaleOf(s32 grade);
    static s32 gradeColorIndex(s32 grade);

    /** Everything a seat's meter shows, for tests and the drawing. */
    struct Look {
        bool shown = false;
        s32 grade = -1;
        s32 count = 0;
        s32 best = 0;
        f32 fill = 0;      ///< the bar's share
        f32 presence = 0;  ///< 0 away to 1 in place
        f32 punch = 0;     ///< a blow's bump, 1 at the blow
        f32 rankFlash = 0; ///< seconds of the new grade's flash left
        f32 recordShow = 0; ///< seconds of the record announcement left
        f32 shake = 0;     ///< burst energy, decaying
        bool ended = false; ///< the run is over, the verdict lingering then fading
        bool operator==(const Look&) const = default;
    };

    /** Takes in the seats' runs for this frame; a seat left out is no longer watched. */
    void observe(std::span<const StreakView> views);
    /** Moves every animation on by `seconds`, at most `kMaxFrame`. */
    void animate(f32 seconds);
    void draw(Canvas& canvas, const TextPainter& text, const Texture* glow,
              const StringTable* strings) const;
    void clear();

    Look look(s32 player) const;
    bool visible() const;

private:
    struct Slot {
        bool watched = false;
        bool running = false;
        s32 count = 0;
        f32 quiet = 0;
        s32 record = 0;
        s32 recordAtStart = 0;
        bool recordBeaten = false;
        s32 grade = -1;
        f32 fill = 0;
        f32 presence = 0;
        f32 punch = 0;
        f32 rankFlash = 0;
        f32 recordShow = 0;
        f32 shake = 0;
        f32 linger = 0;
        f32 time = 0;
        bool ended = false;
        bool shown() const { return presence > 0 || running; }
    };

    static void drawSlot(Canvas& canvas, const TextPainter& text, const Texture* glow,
                         const StringTable* strings, const Slot& slot, s32 row);

    std::array<Slot, 4> m_slots{};
};

} // namespace gdl::game
