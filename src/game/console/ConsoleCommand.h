#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::game {

class DevConsole;

/**
 * One command the developer console runs, as an object: its name, what `help` says of it,
 * what it does with the console and its arguments, and the words it can complete. A
 * command is registered once with the console's registry and found there by name,
 * ignoring case; the application registers its own beside the console's built-in ones.
 */
class ConsoleCommand {
public:
    using Arguments = std::span<const std::string>;

    ConsoleCommand() = default;
    ConsoleCommand(const ConsoleCommand&) = delete;
    ConsoleCommand& operator=(const ConsoleCommand&) = delete;
    ConsoleCommand(ConsoleCommand&&) = delete;
    ConsoleCommand& operator=(ConsoleCommand&&) = delete;
    virtual ~ConsoleCommand() = default;

    virtual std::string_view name() const = 0;
    virtual std::string_view summary() const = 0;
    virtual void run(DevConsole& console, Arguments arguments) = 0;
    /** The words the console may complete the last of `arguments` to (the word being typed,
     * which may be empty); none for a command that takes free text. */
    virtual std::vector<std::string> completions(Arguments arguments) const;
};

/** A command made of a name, a summary and a function: the quick way to register one. */
class LambdaCommand final : public ConsoleCommand {
public:
    using Handler = std::function<void(DevConsole& console, Arguments arguments)>;
    using Completer = std::function<std::vector<std::string>(Arguments arguments)>;

    LambdaCommand(std::string name, std::string summary, Handler handler,
                  Completer completer = {});
    std::string_view name() const override { return m_name; }
    std::string_view summary() const override { return m_summary; }
    void run(DevConsole& console, Arguments arguments) override;
    std::vector<std::string> completions(Arguments arguments) const override;

private:
    std::string m_name;
    std::string m_summary;
    Handler m_handler;
    Completer m_completer;
};

/** A completer that offers a fixed set of words for the first argument. */
LambdaCommand::Completer wordsCompleter(std::vector<std::string> words);

/**
 * The console's commands: kept in the order they were registered (the order `help` lists
 * them), found by name ignoring case, and the first whose name begins with a prefix, for
 * completing the word being typed.
 */
class ConsoleRegistry {
public:
    void add(std::unique_ptr<ConsoleCommand> command);
    /** Registers a command made of a name, a summary and a function. */
    void add(std::string name, std::string summary, LambdaCommand::Handler handler,
             LambdaCommand::Completer completer = {});
    std::span<const std::unique_ptr<ConsoleCommand>> commands() const { return m_commands; }
    usize size() const { return m_commands.size(); }
    /** The command named `name` in any case, or null. */
    ConsoleCommand* find(std::string_view name) const;
    /** The first registered name (among those `allowed`, when given) that begins with
     * `prefix` in any case and is longer than it: what the console completes the word to. */
    std::optional<std::string>
    completeName(std::string_view prefix,
                 const std::function<bool(std::string_view)>& allowed = {}) const;

private:
    std::vector<std::unique_ptr<ConsoleCommand>> m_commands;
};

/** Whether `word` begins with `prefix`, ignoring case. */
bool startsWithIgnoringCase(std::string_view word, std::string_view prefix);
/** Whether two names are the same, ignoring case. */
bool sameNameIgnoringCase(std::string_view a, std::string_view b);

} // namespace gdl::game
