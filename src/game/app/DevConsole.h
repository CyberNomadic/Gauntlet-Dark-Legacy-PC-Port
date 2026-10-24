#pragma once
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/platform/Input.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "game/hud/StatusBox.h"

namespace gdl::game {

/**
 * The developer console: a black DOS-style screen that slides down from the top edge, as wide
 * as the party's four status boxes, and takes the keyboard while it is down. Lines typed at
 * its prompt in the game's 8x8 font, behind a blinking underscore caret, run the commands
 * registered with it; `help` lists them. Left and Right move the caret along the line,
 * Up and Down walk the lines typed before, Page Up and Page Down (or the wheel) the
 * output. The console key slides it down and back up.
 */
class DevConsole {
public:
    using Arguments = std::span<const std::string>;
    using Handler = std::function<void(DevConsole& console, Arguments arguments)>;

    static constexpr s32 kLanes = 4;
    static constexpr s32 kWidth = StatusBoxPainter::kWidth * kLanes; ///< the status strip's
    static constexpr s32 kHeight = 192;                              ///< half the 384-line screen
    static constexpr s32 kMargin = 4;
    /** FONT8X8 cells are eight wide and nine tall; the console writes them at half size. */
    static constexpr f32 kTextScale = 0.5f;
    static constexpr s32 kCellWidth = 4;
    static constexpr s32 kRowHeight = 5;
    static constexpr s32 kColumns = (kWidth - 2 * kMargin) / kCellWidth;
    static constexpr s32 kRows = (kHeight - 2 * kMargin) / kRowHeight; ///< the last is the prompt
    static constexpr usize kScrollback = 400;
    static constexpr usize kHistory = 100;
    static constexpr s32 kWheelLines = 3; ///< output lines a wheel notch scrolls
    static constexpr f64 kSlideSeconds = 0.25;
    /** The VGA text cursor toggles every sixteen retraces of its 70 Hz mode. */
    static constexpr f64 kCaretHalfPeriod = 16.0 / 70.0;
    /** The cursor is the bottom scanline of the cell. */
    static constexpr s32 kCaretHeight = 1;
    /** The BIOS typematic defaults: half a second, then 10.9 repeats a second. */
    static constexpr f64 kRepeatDelay = 0.5;
    static constexpr f64 kRepeatInterval = 1.0 / 10.9;
    static constexpr std::string_view kDefaultPrompt = "GDL>";
    static constexpr std::string_view kBadCommand = "Bad command or file name";
    static constexpr Color kInk = Color::rgba(170, 170, 170); ///< the text mode's light grey
    static constexpr Color kPaper = Color::rgba(0, 0, 0);

    DevConsole();
    ~DevConsole() = default;
    GDL_NON_COPYABLE_NON_MOVABLE(DevConsole);

    /** Loads the 8x8 font from the unpacked assets; without it the screen is drawn blank. */
    bool load(RenderDevice& device, const std::filesystem::path& unpackedRoot);
    void release();
    bool ready() const { return m_text.ready(); }

    /** Registers `name`, matched ignoring case; `summary` is what `help` says of it. */
    void addCommand(std::string name, std::string summary, Handler handler);
    /** Appends output, a line per line break, each wrapped to the screen's columns at
     * spaces where it can be. */
    void print(std::string_view text);
    /** Echoes `line` under the prompt and runs it: the first word names the command, the
     * rest are its arguments. An unknown name is refused the way DOS refused one. */
    void run(std::string_view line);
    /** What stands before the line being typed, such as the character's name and `>`. */
    void setPrompt(std::string prompt) { m_prompt = std::move(prompt); }
    const std::string& prompt() const { return m_prompt; }

    /** Slides the console the other way; coming down, the caret starts shown. */
    void toggle();
    /** Whether the console holds the keyboard: it is down or coming down. */
    bool open() const { return m_open; }
    /** Whether any of the screen shows. */
    bool visible() const { return m_slide > 0.0; }
    /** How far down the screen is, 0 above the view to 1 fully down. */
    f32 slide() const { return static_cast<f32>(m_slide); }
    bool caretShown() const;
    const std::string& line() const { return m_line; }
    /** Where the caret stands in the line: how many characters are before it. */
    usize cursor() const { return m_cursor; }
    std::span<const std::string> output() const { return m_output; }
    std::span<const std::string> history() const { return m_history; }
    /** How many lines above the newest the output is scrolled to. */
    usize scroll() const { return m_scroll; }

    /** Advances the slide and the caret and, while open, takes the frame's keys and text. A
     * press of one of `toggleKeys` slides the console the other way, the character it types
     * dropped; Escape slides it up. Backspace, Left and Right repeat while held. */
    void update(f64 deltaSeconds, const Input& input, std::span<const Key> toggleKeys);
    /** Draws the screen, the output above the prompt line and the caret, in virtual pixels. */
    void draw(Canvas& canvas) const;

private:
    struct Command {
        std::string name;
        std::string summary;
        Handler handler;
    };
    /** A key that repeats while held, at the typematic rate. */
    struct HeldKey {
        Key key = Key::Unknown;
        f64 held = 0.0; ///< how long it has been down
        f64 due = 0.0;  ///< when, on that clock, it next repeats
    };
    void readKeys(f64 deltaSeconds, const Input& input, bool toggled);
    /** How many times `key` acts this poll: once pressed, then by the typematic rate. */
    static s32 strokes(HeldKey& key, const Input& input, f64 deltaSeconds);
    void eraseBeforeCursor();
    void printLine(std::string_view line);
    void recall(s32 step);
    void scrollBy(s32 lines);
    /** How many output lines show above the prompt. */
    usize shownLines() const;
    void help();
    const Command* find(std::string_view name) const;

    BitmapFont m_font;
    TextureSet m_textures;
    TextPainter m_text;
    std::vector<Command> m_commands;
    std::vector<std::string> m_output;
    std::vector<std::string> m_history;
    usize m_recalled = 0;  ///< the history line shown; the count when typing a new one
    std::string m_pending; ///< the new line set aside while an older one is shown
    std::string m_line;
    usize m_cursor = 0; ///< the caret's place in the line
    std::string m_prompt{kDefaultPrompt};
    usize m_scroll = 0;
    bool m_open = false;
    f64 m_slide = 0.0;
    f64 m_caretTime = 0.0;
    HeldKey m_backspace{Key::Backspace};
    HeldKey m_left{Key::Left};
    HeldKey m_right{Key::Right};
};

} // namespace gdl::game
