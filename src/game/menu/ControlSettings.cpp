#include "game/menu/ControlSettings.h"

#include <algorithm>
#include <cctype>

#include "game/config/ControlProfiles.h"
#include "game/menu/SettingsRows.h"

namespace gdl::game {
namespace {
constexpr s32 kDevice = 100;
constexpr s32 kDefaults = 102;
constexpr s32 kApply = 104;
constexpr s32 kRumble = 105;
constexpr s32 kScheme = 106;
constexpr s32 kGestures = 107; ///< the potion key's hold and double tap
constexpr s32 kActionCount = 2;
constexpr s32 kActionY = 268;
constexpr s32 kActionGap = 24;
constexpr s32 kCanvasWidth = 512;
constexpr s32 kCaptureTicks = 600;
// A player's bindings are many rows with values; they share the parchment with the page's
// actions by staying small and scrolling behind a window of a few rows.
constexpr s32 kEditorX = 78;
constexpr s32 kEditorY = 94;
constexpr s32 kEditorRowStep = 18;
constexpr f32 kEditorScale = 0.45f;
constexpr f32 kEditorCursorScale = 0.6f;
constexpr s32 kEditorValueX = 282;
constexpr s32 kEditorValueWidth = 148;
MenuEvent moved() {
    return {MenuAction::Moved, 0};
}
/** Whether the potion key's gestures are on for the device a profile is assigned. */
bool gesturesOf(const PlayerControlConfig& profile, bool keyboard) {
    return keyboard ? profile.play.keyboardMagicGestures : profile.play.padMagicGestures;
}
/** The gesture words, written in a sentence in the strings, as a row's title. */
std::string titleCase(std::string words) {
    bool start = true;
    for (char& c : words) {
        if (start) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        start = c == ' ';
    }
    return words;
}
} // namespace
void ControlSettings::begin(const GameConfig& config) {
    m_draft = config;
    m_navigation = config;
    m_player = -1;
    m_capture = -1;
    m_failed = false;
}
std::vector<PlayerControlConfig> ControlSettings::choices(const GameConfig& config) const {
    std::vector<PlayerControlConfig> result(3);
    result[1].device = "keyboard";
    result[2].device = "none";
    // Player 1 always retains an assignment, including keyboard recovery in Automatic.
    if (m_player == 0) {
        result.pop_back();
    }
    for (usize player = 0; player < config.controls.size(); ++player) {
        if (static_cast<s32>(player) != m_player && config.controls[player].device == "keyboard") {
            result.erase(result.begin() + 1);
            break;
        }
    }
    for (s32 pad = 0; pad < Input::kMaxPads; ++pad) {
        if (const auto* device = m_devices.padDevice(pad)) {
            PlayerControlConfig choice;
            choice.device = device->guid;
            choice.name = device->name;
            choice.occurrence = controllerOccurrence(m_devices, pad);
            bool assigned = false;
            for (usize player = 0; player < config.controls.size(); ++player) {
                const auto& other = config.controls[player];
                assigned |= static_cast<s32>(player) != m_player && other.device == choice.device &&
                            other.occurrence == choice.occurrence;
            }
            if (!assigned) {
                result.push_back(std::move(choice));
            }
        }
    }
    return result;
}
void ControlSettings::define(MenuDefinition& definition, const TextPainter& painter,
                             const std::function<std::string(std::string_view)>& text) const {
    definition.title = text("menu.controls");
    definition.playerLabel.clear();
    definition.fades = false;
    definition.items.clear();
    definition.body.clear();
    definition.itemPositions.clear();
    definition.tailPositions.clear();
    definition.visibleRows = 0;
    SettingsRows rows(definition, painter, text);
    if (m_player < 0) {
        // The players read as the root's choices do: full size, centred on the screen.
        definition.x = -(kCanvasWidth / 2);
        definition.y = -1;
        for (s32 player = 0; player < 4; ++player) {
            rows.choice(text("controls.player") + " " + std::to_string(player + 1), player);
        }
        rows.fit(1.0f, kCanvasWidth);
        return;
    }
    definition.titleScale = 0.75f;
    rows.column(kEditorX, kEditorY, kEditorScale);
    definition.cursorScale = kEditorCursorScale;
    definition.valueX = kEditorValueX;
    definition.valueWidth = kEditorValueWidth;
    definition.visibleRows = kVisibleRows;
    const auto& config = m_draft;
    auto profile = config.controls[static_cast<usize>(m_player)];
    if (!profile.customized) {
        profile.play = config.play;
        profile.menu = config.menu;
    }
    definition.title += " - " + text("controls.player") + " " + std::to_string(m_player + 1);
    std::string device = profile.name;
    if (profile.device.empty()) {
        device = text("controls.auto");
    } else if (profile.device == "keyboard") {
        device = text("controls.keyboard");
    } else if (profile.device == "none") {
        device = text("controls.none");
    } else if (controlDevices(config, m_devices)[static_cast<usize>(m_player)].pad < 0) {
        device = text("controls.disconnected");
    } else {
        device += " " + std::to_string(profile.occurrence + 1);
    }
    // The complete name remains in the selected device's profile; keep the row on parchment.
    if (device.size() > 25) {
        device = device.substr(0, 22) + "...";
    }
    rows.value(text("controls.device"), kDevice, device);
    const auto actions = controlActions();
    const auto assigned = controlDevices(config, m_devices)[static_cast<usize>(m_player)];
    const bool canBind = assigned.keyboard || m_devices.isPadConnected(assigned.pad);
    // The mouse and keyboard's scheme stands at the top of their settings.
    if (assigned.keyboard) {
        rows.value(text("controls.scheme"), kScheme,
                   text(profile.scheme == KeyboardScheme::Robotron ? "controls.robotron"
                                                                   : "controls.tank"));
    }
    const auto buttonLabel = [&](PadButton button) {
        switch (button) {
        case PadButton::LeftStickUp: return text("controls.stickUp");
        case PadButton::LeftStickDown: return text("controls.stickDown");
        case PadButton::LeftStickLeft: return text("controls.stickLeft");
        case PadButton::LeftStickRight: return text("controls.stickRight");
        default: return std::string(padButtonName(button));
        }
    };
    const bool gestures = gesturesOf(profile, assigned.keyboard);
    for (usize index = 0; index < actions.size(); ++index) {
        const auto& action = actions[index];
        const std::string_view actionId = action.id;
        const bool gestureAction = actionId == "throwPotion" || actionId == "shieldPotion";
        std::string value;
        if (m_capture == static_cast<s32>(index)) {
            value = text("controls.press");
        } else if (assigned.keyboard) {
            for (const auto key : action.keyboard(profile)) {
                if (!value.empty()) {
                    value += "/";
                }
                value += keyName(key);
            }
            if (profile.device.empty() && !action.controller(profile).empty()) {
                value += " / ";
                value += buttonLabel(action.controller(profile).front());
            }
            if (value.empty() && gestures && gestureAction && !profile.play.usePotion.empty()) {
                value = std::string(keyName(profile.play.usePotion.front())) + " (" +
                        text(actionId == "throwPotion" ? "controls.hold" : "controls.doubleTap") +
                        ")";
            }
        } else {
            for (const auto button : action.controller(profile)) {
                if (!value.empty()) {
                    value += "/";
                }
                value += buttonLabel(button);
            }
            if (value.empty() && gestures && gestureAction && !profile.play.padUsePotion.empty()) {
                value = buttonLabel(profile.play.padUsePotion.front()) + " (" +
                        text(actionId == "throwPotion" ? "controls.hold" : "controls.doubleTap") +
                        ")";
            }
        }
        if (value.empty()) {
            value = text("controls.unbound");
        }
        if (value.size() > 25) {
            value = value.substr(0, 22) + "...";
        }
        rows.value(text("controls." + std::string(action.id)), static_cast<s32>(index), value,
                   canBind);
    }
    // The potion key's gestures: a hold throws and a double tap shields, as on a pad.
    rows.toggle(titleCase(text("controls.hold")) + " / " + titleCase(text("controls.doubleTap")),
                kGestures, gestures)
        .enabled = canBind;
    if (const auto* pad = m_devices.padDevice(assigned.pad);
        pad != nullptr && pad->rumbleSupported) {
        rows.toggle(text("controls.rumble"), kRumble, profile.rumble);
    }
    // The rows keep the editor's own step; a value too wide for its column is cut short.
    const s32 lineHeight = painter.lineHeight(definition.scale);
    for (auto& item : definition.items) {
        item.extraSpacing = std::max(0, kEditorRowStep - lineHeight);
        auto& value = item.value;
        if (painter.measure(value, definition.scale) > definition.valueWidth) {
            while (!value.empty() &&
                   painter.measure(value + "...", definition.scale) > definition.valueWidth) {
                usize end = value.size() - 1;
                while (end > 0 && (static_cast<u8>(value[end]) & 0xC0U) == 0x80U) {
                    --end;
                }
                value.resize(end);
            }
            value += "...";
        }
    }
    // Apply and Restore Defaults stand in a row under the window, centred.
    rows.choice(text("settings.apply"), kApply);
    rows.choice(text("settings.defaults"), kDefaults);
    const auto firstAction = definition.items.size() - kActionCount;
    s32 width = kActionGap * (kActionCount - 1);
    for (usize i = firstAction; i < definition.items.size(); ++i) {
        width += painter.measure(definition.items[i].text, definition.scale);
    }
    s32 x = (kCanvasWidth - width) / 2;
    for (usize i = firstAction; i < definition.items.size(); ++i) {
        definition.tailPositions.emplace_back(static_cast<f32>(x), kActionY);
        x += painter.measure(definition.items[i].text, definition.scale) + kActionGap;
    }
    if (m_failed || m_capture >= 0) {
        definition.body = {text(m_failed ? "settings.failed" : "controls.cancel")};
        definition.bodyY = 304;
        definition.bodyScale = 0.4f;
    }
}
ControlSettings::Result
ControlSettings::update(const MenuInput& input, s32 ticks, OptionMenu& menu, GameConfig& config,
                        const std::function<bool(const GameConfig&)>& persist,
                        const std::function<void(s32)>& rebuild) {
    bool devicesChanged = false;
    if (input.devices != nullptr) {
        for (s32 pad = 0; pad < Input::kMaxPads; ++pad) {
            const auto* old = m_devices.padDevice(pad);
            const auto* next = input.devices->padDevice(pad);
            devicesChanged |= (old == nullptr) != (next == nullptr) ||
                              (old != nullptr && next != nullptr &&
                               (old->guid != next->guid || old->name != next->name ||
                                old->rumbleSupported != next->rumbleSupported));
        }
        m_devices = *input.devices;
    }
    if (devicesChanged) {
        // An optional hardware row may appear/disappear. Preserve the selected
        // action by identity so a hot-plug cannot turn Apply into Back (or vice versa).
        const s32 code = menu.definition().items[static_cast<usize>(menu.selection())].code;
        rebuild(0);
        const auto& items = menu.definition().items;
        const auto selected = std::ranges::find(items, code, &MenuItem::code);
        if (selected != items.end()) {
            menu.focus(static_cast<usize>(selected - items.begin()));
        }
    }
    if (m_capture >= 0) {
        m_timeout -= std::max(0, ticks);
        const auto device = controlDevices(m_draft, m_devices)[static_cast<usize>(m_player)];
        if (m_timeout <= 0 || m_devices.wasKeyPressed(Key::Escape)) {
            m_capture = -1;
            rebuild(menu.selection());
            return {};
        }
        bool held = false;
        std::optional<Key> key;
        std::optional<PadButton> button;
        for (s32 i = 1; i < static_cast<s32>(Key::Count); ++i) {
            const auto candidate = static_cast<Key>(i);
            held |= device.keyboard && m_devices.isKeyDown(candidate);
            if (device.keyboard && m_devices.wasKeyPressed(candidate)) {
                key = candidate;
            }
        }
        for (s32 pad = 0; pad < Input::kMaxPads; ++pad) {
            for (s32 i = 0; i < static_cast<s32>(PadButton::Count); ++i) {
                const auto candidate = static_cast<PadButton>(i);
                held |= (pad == device.pad || m_devices.wasPadButtonPressed(pad, candidate)) &&
                        m_devices.isPadButtonDown(pad, candidate);
                if (pad == device.pad && m_devices.wasPadButtonPressed(pad, candidate)) {
                    button = candidate;
                }
            }
        }
        held |= m_devices.pointer().down || m_devices.isKeyDown(Key::Enter);
        if (!m_released) {
            m_released = !held;
            return {};
        }
        if (key || button) {
            auto& profile = m_draft.controls[static_cast<usize>(m_player)];
            if (!profile.customized) {
                profile.play = config.play;
                profile.menu = config.menu;
            }
            profile.customized = true;
            const s32 bound = m_capture;
            const auto& action = controlActions()[static_cast<usize>(bound)];
            if (key) {
                action.keyboard(profile) = {*key};
            }
            if (button) {
                action.controller(profile) = {*button};
            }
            m_capture = -1;
            rebuild(menu.selection());
            return {{MenuAction::Choice, bound}};
        }
        return {};
    }
    auto mapped = input;
    // Keep the session's original controls alive even after Apply changes its owner.
    if (input.devices != nullptr) {
        const auto recovery = readSharedMenuInput(m_devices, m_navigation);
        mapped.up |= recovery.up;
        mapped.down |= recovery.down;
        mapped.left |= recovery.left;
        mapped.right |= recovery.right;
        mapped.select |= recovery.select;
        mapped.back |= recovery.back;
    }
    // A fixed keyboard escape route survives changing the device/menu bindings in this editor.
    mapped.back |= m_devices.wasKeyPressed(Key::Escape);
    mapped.up |= m_devices.wasKeyPressed(Key::Up);
    mapped.down |= m_devices.wasKeyPressed(Key::Down);
    mapped.left |= m_devices.wasKeyPressed(Key::Left);
    mapped.right |= m_devices.wasKeyPressed(Key::Right);
    mapped.select |= m_devices.wasKeyPressed(Key::Enter);
    const auto firstAction = static_cast<s32>(menu.definition().items.size()) - kActionCount;
    if (m_player >= 0 && menu.selection() >= firstAction && !mapped.back && !mapped.select) {
        if (mapped.left || mapped.right) {
            rebuild(firstAction +
                    (menu.selection() - firstAction + (mapped.left ? -1 : 1) + kActionCount) %
                        kActionCount);
            return {moved()};
        }
        if (mapped.up || mapped.down) {
            rebuild(mapped.up ? firstAction - 1 : 0);
            return {moved()};
        }
    }
    auto event = menu.update(mapped, ticks);
    const bool turned =
        (mapped.left || mapped.right) && !mapped.back && !mapped.select && m_player >= 0;
    if (turned) {
        const auto code = menu.definition().items[static_cast<usize>(menu.selection())].code;
        if (code == kDevice || code == kRumble || code == kScheme || code == kGestures) {
            event = {MenuAction::Choice, code, 0, mapped.left ? -1 : 1};
        }
    }
    if (event.action == MenuAction::Back) {
        if (m_player < 0) {
            return {{MenuAction::Back, 0}, true};
        }
        const s32 player = m_player;
        m_player = -1;
        m_draft = config;
        m_failed = false;
        rebuild(player);
        return {{MenuAction::Back, 0}};
    }
    if (event.action != MenuAction::Choice) {
        return {event};
    }
    if (m_player < 0) {
        m_player = event.code;
        m_draft = config;
        m_failed = false;
        rebuild(0);
        return {event};
    }
    // A value turned with an arrow is heard as a move, like the other pages' values.
    const MenuEvent heard = turned ? moved() : event;
    if (event.code == kGestures) {
        auto& profile = m_draft.controls[static_cast<usize>(m_player)];
        if (!profile.customized) {
            profile.play = config.play;
            profile.menu = config.menu;
        }
        profile.customized = true;
        const auto device = controlDevices(m_draft, m_devices)[static_cast<usize>(m_player)];
        bool& gestures =
            device.keyboard ? profile.play.keyboardMagicGestures : profile.play.padMagicGestures;
        gestures = !gestures;
        m_failed = false;
        rebuild(menu.selection());
    } else if (event.code == kRumble) {
        auto& profile = m_draft.controls[static_cast<usize>(m_player)];
        profile.rumble = !profile.rumble;
        m_failed = false;
        rebuild(menu.selection());
    } else if (event.code == kScheme) {
        auto& profile = m_draft.controls[static_cast<usize>(m_player)];
        profile.scheme = profile.scheme == KeyboardScheme::Tank ? KeyboardScheme::Robotron
                                                                : KeyboardScheme::Tank;
        m_failed = false;
        rebuild(menu.selection());
    } else if (event.code == kDevice) {
        auto& profile = m_draft.controls[static_cast<usize>(m_player)];
        const auto devices = choices(m_draft);
        const auto found = std::ranges::find_if(devices, [&](const auto& choice) {
            return choice.device == profile.device && choice.occurrence == profile.occurrence;
        });
        const auto count = static_cast<s32>(devices.size());
        const auto index = found == devices.end() ? 0 : static_cast<s32>(found - devices.begin());
        const auto& chosen =
            devices[static_cast<usize>((index + (event.direction < 0 ? -1 : 1) + count) % count)];
        profile.device = chosen.device;
        profile.name = chosen.name;
        profile.occurrence = chosen.occurrence;
        m_failed = false;
        rebuild(menu.selection());
    } else if (event.code == kApply) {
        auto next = config;
        next.controls[static_cast<usize>(m_player)] =
            m_draft.controls[static_cast<usize>(m_player)];
        const auto devices = controlDevices(next, m_devices);
        const bool usable = std::ranges::any_of(devices, [&](const auto& device) {
            return device.keyboard || m_devices.isPadConnected(device.pad);
        });
        m_failed = next.controls[0].device == "none" || !usable || !persist || !persist(next);
        if (!m_failed) {
            config = next;
            m_draft = next;
        }
        rebuild(menu.selection());
    } else if (event.code == kDefaults) {
        auto& profile = m_draft.controls[static_cast<usize>(m_player)];
        profile.play = PlayBindings{};
        profile.menu = MenuBindings{};
        profile.rumble = true;
        profile.scheme = KeyboardScheme::Tank;
        profile.customized = true;
        m_failed = false;
        rebuild(menu.selection());
    } else if (event.code >= 0 && event.code < static_cast<s32>(controlActions().size())) {
        m_capture = event.code;
        m_failed = false;
        m_released = false;
        m_timeout = kCaptureTicks;
        rebuild(menu.selection());
    }
    return {heard};
}
} // namespace gdl::game
