#include "game/app/DevConsole.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <exception>
#include <format>
#include <utility>

#include "engine/core/Log.h"

namespace gdl::game {

namespace {
constexpr std::string_view kFontFile = "fonts/font8x8.json";
constexpr std::string_view kTextureDirectory = "STATIC";
constexpr std::string_view kTextureName = "FONT8X8";
constexpr s32 kSpaceWidth = 8;
constexpr u32 kFirstTypable = 0x20;
constexpr u32 kLastTypable = 0x7E;

bool sameNameIgnoringCase(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}

std::vector<std::string> words(std::string_view line) {
    std::vector<std::string> result;
    usize at = 0;
    while (at < line.size()) {
        const usize start = line.find_first_not_of(' ', at);
        if (start == std::string_view::npos) {
            break;
        }
        const usize end = std::min(line.find(' ', start), line.size());
        result.emplace_back(line.substr(start, end - start));
        at = end;
    }
    return result;
}
} // namespace

DevConsole::DevConsole() {
    addCommand("help", "lists the commands",
               [](DevConsole& console, Arguments /*arguments*/) { console.help(); });
}

bool DevConsole::load(RenderDevice& device, const std::filesystem::path& unpackedRoot) {
    release();
    if (!m_font.load(unpackedRoot / kFontFile, kSpaceWidth) ||
        !m_textures.load(unpackedRoot / kTextureDirectory)) {
        return false;
    }
    const auto index = m_textures.find(kTextureName);
    if (!index.has_value()) {
        return false;
    }
    try {
        m_text.setFont(&m_font, &m_textures.texture(device, *index));
    } catch (const std::exception& e) {
        log::warn("Developer console: texture {}: {}", kTextureName, e.what());
        return false;
    }
    return true;
}

void DevConsole::release() {
    m_text.setFont(nullptr, nullptr);
    m_textures.releaseTextures();
}

void DevConsole::addCommand(std::string name, std::string summary, Handler handler) {
    m_commands.push_back(Command{std::move(name), std::move(summary), std::move(handler)});
}

void DevConsole::print(std::string_view text) {
    do {
        const usize length = std::min<usize>(text.size(), static_cast<usize>(kColumns));
        m_output.emplace_back(text.substr(0, length));
        text.remove_prefix(length);
    } while (!text.empty());
    if (m_output.size() > kScrollback) {
        m_output.erase(m_output.begin(), m_output.begin() + static_cast<std::ptrdiff_t>(
                                                                m_output.size() - kScrollback));
    }
}

void DevConsole::run(std::string_view line) {
    print(std::format("{}{}", kPrompt, line));
    const std::vector<std::string> parts = words(line);
    if (parts.empty()) {
        return;
    }
    if (const Command* command = find(parts.front())) {
        command->handler(*this, Arguments{parts}.subspan(1));
    } else {
        print(kBadCommand);
    }
}

const DevConsole::Command* DevConsole::find(std::string_view name) const {
    const auto found = std::ranges::find_if(m_commands, [&](const Command& command) {
        return sameNameIgnoringCase(command.name, name);
    });
    return found != m_commands.end() ? &*found : nullptr;
}

void DevConsole::help() {
    usize widest = 0;
    for (const Command& command : m_commands) {
        widest = std::max(widest, command.name.size());
    }
    for (const Command& command : m_commands) {
        print(std::format("{:<{}}  {}", command.name, widest, command.summary));
    }
}

void DevConsole::toggle() {
    m_open = !m_open;
    if (m_open) {
        m_caretTime = 0.0;
    }
    m_backspaceHeld = 0.0;
}

bool DevConsole::caretShown() const {
    return std::fmod(m_caretTime, 2.0 * kCaretHalfPeriod) < kCaretHalfPeriod;
}

void DevConsole::update(f64 deltaSeconds, const Input& input, std::span<const Key> toggleKeys) {
    const bool toggled =
        std::ranges::any_of(toggleKeys, [&](Key key) { return input.wasKeyPressed(key); });
    if (toggled) {
        toggle();
    }
    if (m_open) {
        readKeys(deltaSeconds, input, toggled);
    }
    m_slide =
        std::clamp(m_slide + (m_open ? deltaSeconds : -deltaSeconds) / kSlideSeconds, 0.0, 1.0);
    m_caretTime += deltaSeconds;
}

void DevConsole::readKeys(f64 deltaSeconds, const Input& input, bool toggled) {
    if (input.wasKeyPressed(Key::Escape)) {
        toggle();
        return;
    }
    const usize room = static_cast<usize>(kColumns) - kPrompt.size() - 1; // a cell for the caret
    if (!toggled) {
        for (const u32 code : input.typedText()) {
            if (code >= kFirstTypable && code <= kLastTypable && m_line.size() < room) {
                m_line.push_back(static_cast<char>(code));
            }
        }
    }
    if (input.wasKeyPressed(Key::Backspace)) {
        erase(1);
        m_backspaceHeld = 0.0;
        m_repeatDue = kRepeatDelay;
    } else if (input.isKeyDown(Key::Backspace)) {
        m_backspaceHeld += deltaSeconds;
        while (m_backspaceHeld >= m_repeatDue) {
            erase(1);
            m_repeatDue += kRepeatInterval;
        }
    } else {
        m_backspaceHeld = 0.0;
    }
    if (input.wasKeyPressed(Key::Enter)) {
        const std::string line = std::move(m_line);
        m_line.clear();
        run(line);
    }
}

void DevConsole::erase(usize count) {
    m_line.resize(m_line.size() - std::min(count, m_line.size()));
}

void DevConsole::draw(Canvas& canvas) const {
    const auto top = static_cast<f32>(std::round(-static_cast<f64>(kHeight) * (1.0 - m_slide)));
    canvas.fill(Rect{0.0f, top, static_cast<f32>(kWidth), static_cast<f32>(kHeight)}, kPaper);
    if (!ready()) {
        return;
    }
    const auto rowY = [&](usize row) {
        return static_cast<s32>(top) + kMargin + static_cast<s32>(row) * kRowHeight;
    };
    const TextStyle style{.color = kInk};
    const usize promptRow = std::min(m_output.size(), static_cast<usize>(kRows - 1));
    const usize first = m_output.size() - promptRow;
    for (usize row = 0; row < promptRow; ++row) {
        m_text.draw(canvas, kMargin, rowY(row), m_output[first + row], style);
    }
    const s32 y = rowY(promptRow);
    s32 x = m_text.draw(canvas, kMargin, y, kPrompt, style);
    x = m_text.draw(canvas, x, y, m_line, style);
    if (caretShown()) {
        canvas.fill(Rect{static_cast<f32>(x), static_cast<f32>(y + kRowHeight - kCaretHeight),
                         static_cast<f32>(kCellWidth), static_cast<f32>(kCaretHeight)},
                    kInk);
    }
}

} // namespace gdl::game
