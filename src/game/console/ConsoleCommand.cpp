#include "game/console/ConsoleCommand.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include "engine/core/Types.h"

namespace gdl::game {

bool sameNameIgnoringCase(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}

bool startsWithIgnoringCase(std::string_view word, std::string_view prefix) {
    return word.size() >= prefix.size() &&
           sameNameIgnoringCase(word.substr(0, prefix.size()), prefix);
}

std::vector<std::string> ConsoleCommand::completions(Arguments /*arguments*/) const {
    return {};
}

LambdaCommand::LambdaCommand(std::string name, std::string summary, Handler handler,
                             Completer completer)
    : m_name(std::move(name)), m_summary(std::move(summary)), m_handler(std::move(handler)),
      m_completer(std::move(completer)) {}

void LambdaCommand::run(DevConsole& console, Arguments arguments) {
    if (m_handler) {
        m_handler(console, arguments);
    }
}

std::vector<std::string> LambdaCommand::completions(Arguments arguments) const {
    return m_completer ? m_completer(arguments) : std::vector<std::string>{};
}

LambdaCommand::Completer wordsCompleter(std::vector<std::string> words) {
    return [words = std::move(words)](ConsoleCommand::Arguments arguments) {
        return arguments.size() <= 1 ? words : std::vector<std::string>{};
    };
}

void ConsoleRegistry::add(std::unique_ptr<ConsoleCommand> command) {
    if (command == nullptr) {
        return;
    }
    // A name registered again replaces the command that held it, where it stood.
    const auto held = std::ranges::find_if(m_commands, [&](const auto& other) {
        return sameNameIgnoringCase(other->name(), command->name());
    });
    if (held != m_commands.end()) {
        *held = std::move(command);
    } else {
        m_commands.push_back(std::move(command));
    }
}

void ConsoleRegistry::add(std::string name, std::string summary, LambdaCommand::Handler handler,
                          LambdaCommand::Completer completer) {
    add(std::make_unique<LambdaCommand>(std::move(name), std::move(summary), std::move(handler),
                                        std::move(completer)));
}

ConsoleCommand* ConsoleRegistry::find(std::string_view name) const {
    const auto found = std::ranges::find_if(m_commands, [&](const auto& command) {
        return sameNameIgnoringCase(command->name(), name);
    });
    return found != m_commands.end() ? found->get() : nullptr;
}

std::optional<std::string>
ConsoleRegistry::completeName(std::string_view prefix,
                              const std::function<bool(std::string_view)>& allowed) const {
    if (prefix.empty()) {
        return std::nullopt;
    }
    for (const auto& command : m_commands) {
        const std::string_view name = command->name();
        if (name.size() > prefix.size() && startsWithIgnoringCase(name, prefix) &&
            (!allowed || allowed(name))) {
            return std::string(name);
        }
    }
    return std::nullopt;
}

} // namespace gdl::game
