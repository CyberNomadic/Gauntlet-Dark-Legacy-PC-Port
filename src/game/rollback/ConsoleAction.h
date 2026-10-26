#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#include "engine/core/Types.h"

namespace gdl::game {

/** A bounded console mutation admitted by the host, then recorded as a one-shot input.
 * The seat is the player the command speaks for, not the input seat carrying it.
 * Parsing command arguments and applying them belongs to the native scene. */
struct ConsoleAction {
    static constexpr usize kMaxLine = 160;
    u8 seat = 0;
    std::string line;

    bool valid() const {
        if (seat >= 4 || line.empty() || line.size() > kMaxLine ||
            !std::ranges::all_of(line, [](unsigned char c) { return c >= 32 && c <= 126; })) {
            return false;
        }
        std::string_view words{line};
        const auto next = [&]() {
            const auto begin = words.find_first_not_of(' ');
            if (begin == std::string_view::npos) {
                words = {};
                return std::string{};
            }
            words.remove_prefix(begin);
            const auto end = words.find(' ');
            std::string word{words.substr(0, end)};
            words = end == std::string_view::npos ? std::string_view{} : words.substr(end);
            for (char& c : word) {
                if (c >= 'A' && c <= 'Z') {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            return word;
        };
        const std::string verb = next();
        constexpr std::array<std::string_view, 8> kVerbs{
            "get", "goto", "unlock_tower", "suicide", "god", "notarget", "noclip", "spawn"};
        return std::ranges::find(kVerbs, verb) != kVerbs.end() &&
               (verb != "get" || next() != "info");
    }
    bool operator==(const ConsoleAction&) const = default;
};

} // namespace gdl::game
