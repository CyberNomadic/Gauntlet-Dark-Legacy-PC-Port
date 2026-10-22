#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "engine/assets/StringTable.h"

namespace gdl::game {

/** Compatibility gate for online play, judged by what a build does rather than which
 * binary it is. The runtime identity digests the sources and version the game was built
 * from (see SourceIdentity.h) and a numeric witness of its arithmetic: the x86-64 profile,
 * rounding and exception modes, and the results of a fixed corpus of the math the
 * simulation uses. Two builds agree when their sources agree and their numbers agree,
 * whatever compiler, machine or operating system made them. The witness is bounded
 * evidence, not a proof for every input; a divergence in play is caught by the periodic
 * digests and repaired by the journal rebuild. Not an authentication mechanism. */
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

    /** Compile-selected runtime linkage, for logs; not part of the identity. */
    static std::string_view runtimeProfile();

    /** Call once at process startup, before Application or any worker threads.
     * Checks the rounding, denormal and exception modes the witness assumes, and on
     * Windows disables the UCRT's CPU-dependent FMA3 math path, including for offline
     * play. Repeated calls validate the established policy rather than repairing a
     * later mode change. */
    static bool prepareProcessNumeric(std::string* error = nullptr);

    /** Call on the simulation thread. The returned value owns the effective text digest
     * and the numeric witness; no StringTable or other live owner is borrowed by
     * capture(). */
    static std::optional<Inputs> snapshot(const StringTable& strings, std::string* error = nullptr);

    /** Combines the witness with the build's source identity. */
    static std::optional<Result> capture(Inputs inputs, std::string* error = nullptr);

    /** Check on the simulation thread before using a completed capture: the witness is
     * recomputed here and now. Floating-point sticky exception flags are not mode state. */
    static bool numericMatches(const Inputs& inputs);
};

} // namespace gdl::game
