#pragma once

#include <optional>
#include <string>

#include "engine/assets/StringTable.h"

namespace gdl::game {

/** Conservative compatibility gate for the native simulation experiment, not
 * an authentication mechanism or a proof of complete determinism. The runtime
 * identity pins the executable, OS/architecture/toolchain, numeric environment
 * and the explicitly identified loaded C/C++ math runtimes. Cross-platform
 * agreement is deliberately not implied. No native asset tree is read here. */
class NativeOnlineIdentity {
public:
    struct Inputs {
        std::string data;
        std::string language;
        std::string numeric;
    };
    struct Result {
        std::string runtime;
        std::string data;
        std::string language;
    };

    /** Call once at process startup, before Application or any worker threads.
     * On Windows this disables the UCRT's CPU-dependent FMA3 math path, including
     * for offline play. It never changes rounding, denormal or exception modes.
     * Repeated calls validate the established policy rather than repairing a
     * later mode change. Other platforms retain their conservative CPU gate. */
    static bool prepareProcessNumeric(std::string* error = nullptr);

    /** Call on the simulation thread before starting the background hash. The
     * returned value owns the effective text digest and numeric-mode witness;
     * no StringTable or other live owner is borrowed by capture(). Windows uses
     * an exact operation corpus instead of CPU model identity, with the same
     * executable/runtime still required. Corpus agreement is bounded evidence,
     * not a proof for every possible input or a cross-platform guarantee. */
    static std::optional<Inputs> snapshot(const StringTable& strings, std::string* error = nullptr);

    /** Load-time/background work: stream the actual executable and known loaded
     * runtime files. Missing, unreadable or changing files fail without falling
     * back to a release version, cwd, guessed library name or local path hash. */
    static std::optional<Result> capture(Inputs inputs, std::string* error = nullptr);

    /** Check on the simulation thread before using a completed background hash.
     * Floating-point sticky exception flags are intentionally not mode state. */
    static bool numericMatches(const Inputs& inputs);
};

} // namespace gdl::game
