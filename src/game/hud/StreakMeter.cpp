#include "game/hud/StreakMeter.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

#include "engine/assets/StringTable.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "game/players/MeleeStreak.h"

namespace gdl::game {

namespace {

constexpr s32 kBurstShards = 8;
constexpr s32 kConfetti = 12;
constexpr f32 kSlideIn = 48.0f; ///< pixels the meter slides in from
constexpr f32 kGlowExpand = 3.0f;

/** A smooth, deterministic jitter in [-1, 1]; lanes keep the axes from moving together. */
f32 wobble(f32 t, s32 lane) {
    const auto offset = static_cast<f32>(lane);
    return std::sin(t * 1.0f + offset * 2.3f) * std::cos(t * 0.61f + offset * 1.1f);
}

f32 smooth(f32 x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

/** Ease out past the mark and settle: the meter overshoots as it lands. */
f32 overshoot(f32 x) {
    x = std::clamp(x, 0.0f, 1.0f);
    const f32 back = 1.7f;
    const f32 y = x - 1.0f;
    return 1.0f + y * y * ((back + 1.0f) * y + back);
}

Color mix(Color a, Color b, f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const auto channel = [t](u8 from, u8 to) {
        return static_cast<u8>(std::lround(static_cast<f32>(from) +
                                           (static_cast<f32>(to) - static_cast<f32>(from)) * t));
    };
    return Color::rgba(channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b),
                       channel(a.a, b.a));
}

Color faded(Color color, f32 alpha) {
    const f32 scaled = static_cast<f32>(color.a) * std::clamp(alpha, 0.0f, 1.0f);
    return color.withAlpha(static_cast<u8>(std::lround(scaled)));
}

std::string_view label(const StringTable* strings, std::string_view id, std::string_view fallback) {
    return strings != nullptr && strings->has(id) ? strings->get(id) : fallback;
}

std::string numbered(std::string_view pattern, s32 number) {
    return std::vformat(pattern, std::make_format_args(number));
}

} // namespace

s32 StreakMeter::gradeOf(s32 count) {
    s32 grade = -1;
    for (s32 i = 0; i < kGrades; ++i) {
        if (count >= kGradeCounts[static_cast<usize>(i)]) {
            grade = i;
        }
    }
    return grade;
}

f32 StreakMeter::bandProgress(s32 count) {
    const s32 grade = gradeOf(count);
    if (grade < 0) {
        return 0.0f;
    }
    if (grade == kGrades - 1) {
        return 1.0f;
    }
    const s32 low = kGradeCounts[static_cast<usize>(grade)];
    const s32 high = kGradeCounts[static_cast<usize>(grade) + 1];
    return static_cast<f32>(count - low) / static_cast<f32>(high - low);
}

f32 StreakMeter::fillOf(s32 count, f32 quiet) {
    if (count <= 0) {
        return 0.0f;
    }
    // The climb through the grade sets the bar's reach; the quiet drains it to nothing.
    const f32 reach = 0.12f + 0.88f * bandProgress(count);
    const f32 life = std::clamp(1.0f - quiet / StreakGrades::quietWindow(count), 0.0f, 1.0f);
    return reach * life;
}

f32 StreakMeter::shakeOf(s32 grade) {
    constexpr std::array<f32, kGrades> kJitter{0.0f, 0.4f, 0.8f, 1.4f, 2.2f};
    return grade < 0 ? 0.0f : kJitter[static_cast<usize>(grade)];
}

f32 StreakMeter::letterScaleOf(s32 grade) {
    return 1.5f + 0.125f * static_cast<f32>(std::max(grade, 0));
}

s32 StreakMeter::gradeColorIndex(s32 grade) {
    return std::clamp(grade, 0, kGrades - 1);
}

void StreakMeter::observe(std::span<const StreakView> views) {
    for (Slot& slot : m_slots) {
        slot.watched = false;
    }
    for (const StreakView& view : views) {
        if (view.player < 0 || view.player >= static_cast<s32>(m_slots.size())) {
            continue;
        }
        Slot& slot = m_slots[static_cast<usize>(view.player)];
        slot.watched = true;
        if (view.running) {
            if (!slot.running) {
                // A run begins: the record it has to beat is the one standing before it.
                slot.recordAtStart = slot.record;
                slot.recordBeaten = false;
                slot.ended = false;
                slot.linger = 0.0f;
            }
            const s32 grade = gradeOf(view.count);
            if (view.count > slot.count) {
                slot.punch = 1.0f;
                slot.shake += 0.35f + 0.2f * static_cast<f32>(std::max(grade, 0));
                if (grade > slot.grade && slot.grade >= 0) {
                    slot.rankFlash = kRankFlash;
                    slot.shake += 1.0f + 0.3f * static_cast<f32>(grade);
                }
                if (!slot.recordBeaten && slot.recordAtStart > 0 && view.count > slot.recordAtStart) {
                    slot.recordBeaten = true;
                    slot.recordShow = kRecordShow;
                    slot.shake += 1.5f;
                }
            }
            slot.count = view.count;
            slot.grade = grade;
            slot.quiet = view.quiet;
        } else if (slot.running) {
            // The run is over: the verdict slams and lingers before the meter leaves.
            slot.ended = true;
            slot.linger = kLinger;
            slot.punch = 1.0f;
            slot.shake += 0.8f + 0.3f * static_cast<f32>(std::max(slot.grade, 0));
            slot.quiet = MeleeStreak::kQuiet;
        }
        slot.running = view.running;
        slot.record = view.record;
    }
    for (Slot& slot : m_slots) {
        if (!slot.watched) {
            slot.running = false;
            slot.ended = false;
            slot.linger = 0.0f;
        }
    }
}

void StreakMeter::animate(f32 seconds) {
    seconds = std::clamp(seconds, 0.0f, kMaxFrame);
    for (Slot& slot : m_slots) {
        slot.time += seconds;
        if (slot.ended) {
            slot.linger = std::max(slot.linger - seconds, 0.0f);
        }
        const bool present = slot.running || (slot.ended && slot.linger > 0.0f);
        slot.presence = present ? std::min(slot.presence + seconds * kAppearRate, 1.0f)
                                : std::max(slot.presence - seconds * kFadeRate, 0.0f);
        slot.punch = std::max(slot.punch - seconds * kPunchDecay, 0.0f);
        slot.rankFlash = std::max(slot.rankFlash - seconds, 0.0f);
        slot.recordShow = std::max(slot.recordShow - seconds, 0.0f);
        slot.shake = std::max(slot.shake * (1.0f - seconds * kShakeDecay), 0.0f);
        // The quiet itself paces the drain; a blow lifts the bar at once.
        slot.fill = slot.running ? fillOf(slot.count, slot.quiet) : 0.0f;
        if (!slot.running && slot.presence <= 0.0f) {
            const s32 record = slot.record;
            const bool watched = slot.watched;
            const f32 time = slot.time;
            slot = {};
            slot.record = record;
            slot.watched = watched;
            slot.time = time;
        }
    }
}

void StreakMeter::clear() {
    m_slots = {};
}

StreakMeter::Look StreakMeter::look(s32 player) const {
    if (player < 0 || player >= static_cast<s32>(m_slots.size())) {
        return {};
    }
    const Slot& slot = m_slots[static_cast<usize>(player)];
    return {.shown = slot.shown(),
            .grade = slot.grade,
            .count = slot.count,
            .best = slot.record,
            .fill = slot.fill,
            .presence = slot.presence,
            .punch = slot.punch,
            .rankFlash = slot.rankFlash,
            .recordShow = slot.recordShow,
            .shake = slot.shake,
            .ended = slot.ended};
}

bool StreakMeter::visible() const {
    return std::ranges::any_of(m_slots, [](const Slot& slot) { return slot.shown(); });
}

void StreakMeter::draw(Canvas& canvas, const TextPainter& text, const Texture* glow,
                       const StringTable* strings) const {
    if (!text.ready()) {
        return;
    }
    s32 row = 0;
    for (const Slot& slot : m_slots) {
        if (!slot.shown() || slot.grade < 0) {
            continue;
        }
        drawSlot(canvas, text, glow, strings, slot, row);
        ++row;
    }
}

void StreakMeter::drawSlot(Canvas& canvas, const TextPainter& text, const Texture* glow,
                           const StringTable* strings, const Slot& slot, s32 row) {
    const f32 alpha = smooth(slot.presence);
    if (alpha <= 0.0f) {
        return;
    }
    const s32 colorIndex = gradeColorIndex(slot.grade);
    const Color base = kGradeColors[static_cast<usize>(colorIndex)];
    const f32 jitter = shakeOf(slot.grade) + slot.shake * 3.0f;
    const f32 t = slot.time * 40.0f;
    const f32 shakeX = jitter * wobble(t, 0);
    const f32 shakeY = jitter * wobble(t, 1);
    const f32 slide = (1.0f - overshoot(slot.presence)) * kSlideIn;
    const f32 left = static_cast<f32>(kX) - slide + shakeX;
    const f32 top = static_cast<f32>(kY + row * kRowHeight) + shakeY;

    // The letter, bumped by every blow and slammed by the verdict, kept on its centre.
    const std::string_view letter = kLetters.substr(static_cast<usize>(colorIndex), 1);
    const f32 restScale = letterScaleOf(slot.grade);
    const f32 slam = slot.ended ? 0.45f * (slot.linger / kLinger) : 0.0f;
    const f32 scale = restScale * (1.0f + 0.35f * slot.punch + slam);
    const f32 restWidth = static_cast<f32>(text.measure(letter, restScale));
    const f32 restHeight = static_cast<f32>(text.lineHeight(restScale));
    const f32 width = static_cast<f32>(text.measure(letter, scale));
    const f32 height = static_cast<f32>(text.lineHeight(scale));
    const f32 letterX = left - (width - restWidth) * 0.5f;
    const f32 letterY = top - (height - restHeight) * 0.5f;
    const f32 centreX = left + restWidth * 0.5f;
    const f32 centreY = top + restHeight * 0.5f;

    // Blood runs from the letter, more of it the higher the grade, before the letter itself.
    const s32 drips = colorIndex + 1;
    const f32 bottom = top + restHeight - 3.0f;
    for (s32 i = 0; i < drips; ++i) {
        const auto lane = static_cast<f32>(i);
        const f32 x = left + (lane + 0.5f) * restWidth / static_cast<f32>(drips) +
                      wobble(slot.time * 3.0f, i + 2);
        const f32 pulse = 0.65f + 0.35f * std::sin(slot.time * 2.0f + lane * 1.9f);
        const f32 length = (6.0f + 9.0f * static_cast<f32>(colorIndex)) * pulse +
                           18.0f * slot.punch;
        const Color blood = faded(kBlood, alpha * (0.6f + 0.4f * pulse));
        canvas.fill(Rect{x, bottom, 2.0f, length}, blood);
        canvas.fillTriangle(Vec2{x - 1.5f, bottom + length}, Vec2{x + 3.5f, bottom + length},
                            Vec2{x + 1.0f, bottom + length + 4.0f}, blood);
    }

    // A new grade bursts shards from the letter's centre.
    if (slot.rankFlash > 0.0f) {
        const f32 burst = 1.0f - slot.rankFlash / kRankFlash;
        const f32 radius = 10.0f + 70.0f * smooth(burst);
        const Color shard = faded(base, alpha * (1.0f - burst));
        for (s32 i = 0; i < kBurstShards; ++i) {
            const f32 angle = static_cast<f32>(i) * (2.0f * kPi / static_cast<f32>(kBurstShards)) +
                              burst * 0.6f;
            const Vec2 direction{std::cos(angle), std::sin(angle)};
            const Vec2 side{-direction.y, direction.x};
            const Vec2 origin{centreX, centreY};
            const Vec2 tip = origin + direction * (radius + 10.0f);
            const Vec2 tail = origin + direction * radius;
            canvas.fillTriangle(tail + side * 2.5f, tail - side * 2.5f, tip, shard);
        }
    }

    Color letterColor = mix(base, Color::white(), 0.55f);
    Color glowColor = base;
    if (slot.rankFlash > 0.0f) {
        const f32 flash = slot.rankFlash / kRankFlash;
        letterColor = mix(letterColor, Color::white(), flash);
        glowColor = mix(glowColor, Color::white(), flash * 0.6f);
    }
    if (slot.recordShow > 0.0f) {
        const f32 beat = 0.5f + 0.5f * std::sin(slot.time * 18.0f);
        glowColor = mix(glowColor, kRecord, beat);
    }
    if (slot.ended) {
        letterColor = mix(letterColor, kBlood, 0.5f * (1.0f - slot.linger / kLinger));
    }
    if (glow != nullptr) {
        TextStyle halo;
        halo.scale = scale;
        halo.texture = glow;
        halo.color = faded(glowColor, alpha);
        halo.expand = static_cast<s32>(std::lround(kGlowExpand + 3.0f * slot.punch));
        text.draw(canvas, static_cast<s32>(letterX), static_cast<s32>(letterY), letter, halo);
    }
    TextStyle face;
    face.scale = scale;
    face.color = faded(letterColor, alpha);
    text.draw(canvas, static_cast<s32>(letterX), static_cast<s32>(letterY), letter, face);

    // The count over the bar, the bar, and the best under it.
    const f32 barX = left + restWidth + static_cast<f32>(kLetterGap);
    const f32 labelScale = kLabelScale * (1.0f + 0.15f * slot.punch);
    TextStyle count;
    count.scale = labelScale;
    count.color = faded(Color::white(), alpha);
    const auto number = numbered(label(strings, "hud.streakCombo", "COMBO X{}"), slot.count);
    text.draw(canvas, static_cast<s32>(barX), static_cast<s32>(top + 2.0f), number, count);
    const f32 barY = top + 2.0f + static_cast<f32>(text.lineHeight(kLabelScale)) + 3.0f;
    canvas.fill(Rect{barX - 1.0f, barY - 1.0f, static_cast<f32>(kBarWidth) + 2.0f,
                     static_cast<f32>(kBarHeight) + 2.0f},
                faded(Color::black(), alpha * 0.65f));
    const f32 fillWidth = static_cast<f32>(kBarWidth) * std::clamp(slot.fill, 0.0f, 1.0f);
    if (fillWidth > 0.0f) {
        const Color heat = mix(base, kBlood, 0.35f * static_cast<f32>(colorIndex) / 4.0f);
        canvas.fill(Rect{barX, barY, fillWidth, static_cast<f32>(kBarHeight)},
                    faded(mix(heat, Color::white(), slot.punch * 0.7f), alpha));
        canvas.fill(Rect{barX, barY, fillWidth, 2.0f},
                    faded(Color::white(), alpha * (0.25f + 0.5f * slot.punch)));
    }
    TextStyle best;
    best.scale = kBestScale;
    best.color = faded(slot.recordBeaten ? kRecord : Color::rgba(190, 190, 190), alpha);
    text.draw(canvas, static_cast<s32>(barX),
              static_cast<s32>(barY + static_cast<f32>(kBarHeight) + 3.0f),
              numbered(label(strings, "hud.streakBest", "BEST {}"), slot.record), best);

    // A record beaten: the announcement pulses beside the bar in a shower of confetti.
    if (slot.recordShow > 0.0f) {
        const f32 played = 1.0f - slot.recordShow / kRecordShow;
        const f32 fade = std::min(slot.recordShow / 0.5f, 1.0f) * alpha;
        const f32 pulse = 1.0f + 0.25f * std::sin(played * 24.0f) * (1.0f - played);
        const bool blink = std::fmod(slot.time * 8.0f, 2.0f) < 1.0f;
        const f32 recordX = barX + static_cast<f32>(kBarWidth) + 8.0f;
        const f32 recordY = top + 4.0f - 6.0f * smooth(played);
        const std::string_view announcement = label(strings, "hud.streakRecord", "NEW RECORD!");
        if (glow != nullptr) {
            TextStyle halo;
            halo.scale = kRecordScale * pulse;
            halo.texture = glow;
            halo.color = faded(kRecord, fade);
            halo.expand = 2;
            text.draw(canvas, static_cast<s32>(recordX), static_cast<s32>(recordY), announcement,
                      halo);
        }
        TextStyle shout;
        shout.scale = kRecordScale * pulse;
        shout.color = faded(blink ? Color::white() : kRecord, fade);
        text.draw(canvas, static_cast<s32>(recordX), static_cast<s32>(recordY), announcement,
                  shout);
        for (s32 i = 0; i < kConfetti; ++i) {
            const auto lane = static_cast<f32>(i);
            const f32 angle = -0.3f - lane * (2.4f / static_cast<f32>(kConfetti));
            const f32 speed = 50.0f + 30.0f * wobble(lane, 5);
            const Vec2 origin{recordX, recordY + 6.0f};
            const Vec2 at{origin.x + std::cos(angle) * speed * played * 1.6f,
                          origin.y + std::sin(angle) * speed * played + 40.0f * played * played};
            const Color piece = faded(kGradeColors[static_cast<usize>(i % kGrades)], fade);
            const f32 spin = slot.time * 9.0f + lane;
            const Vec2 arm{std::cos(spin) * 2.5f, std::sin(spin) * 2.5f};
            canvas.fillTriangle(at + arm, at - arm, at + Vec2{-arm.y, arm.x} * 1.5f, piece);
        }
    }
}

} // namespace gdl::game
