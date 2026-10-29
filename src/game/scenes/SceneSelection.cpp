#include "game/scenes/SceneSelection.h"

#include <array>
#include <cctype>
#include <charconv>
#include <format>

namespace gdl::game {

namespace {

constexpr std::array<std::string_view, 10> kKindNames{
    "object", "enemy", "critter", "generator", "pickup", "player", "spawned", "switch", "gate",
    "chest"};

bool sameIgnoringCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string_view SceneSelection::kindName(Kind kind) {
    return kKindNames[static_cast<usize>(kind)];
}

std::string SceneSelection::token() const {
    return std::format("{}#{}", kindName(kind), id);
}

std::string SceneSelection::label() const {
    return name.empty() ? token() : std::format("{} {}", token(), name);
}

std::optional<SceneSelection> SceneSelection::parse(std::string_view token) {
    const usize hash = token.find('#');
    if (hash == std::string_view::npos || hash == 0 || hash + 1 >= token.size()) {
        return std::nullopt;
    }
    const std::string_view word = token.substr(0, hash);
    const std::string_view digits = token.substr(hash + 1);
    s32 id = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), id);
    if (error != std::errc{} || end != digits.data() + digits.size() || id < 0) {
        return std::nullopt;
    }
    for (usize i = 0; i < kKindNames.size(); ++i) {
        if (sameIgnoringCase(word, kKindNames[i])) {
            return SceneSelection{.kind = static_cast<Kind>(i), .id = id};
        }
    }
    return std::nullopt;
}

} // namespace gdl::game
