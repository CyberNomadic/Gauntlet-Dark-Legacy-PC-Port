#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::game {

/** Local diagnostic observations of actual PlayScene owners, not a replay seed,
 * restore point, or certificate of whole-scene determinism. Call at a native
 * update boundary on the scene's owning thread, never concurrently with update,
 * rendering, or teardown. SHA-256 equality is probabilistic agreement only.
 *
 * The combat/stage visitors omit only their existing, explicitly normalized
 * draw-overwritten pose caches and point-light lists. No packet is encoded and
 * no ordinary-combat admission test is applied. Native opponent cloud/Death
 * follow-up values are walked directly alongside the represented adapter state;
 * this does not expand that adapter's replay or restore domain. Checkpoint/catalog and
 * canonical-walker limits still apply; a refusal is reported, never an empty
 * state's digest. Render-history inputs and external outputs are not observed. */
struct SimulationObservation {
    using Digest = std::array<u8, 32>;
    struct Subsystem {
        std::string name;
        std::optional<Digest> digest;
        /** Nonempty exactly when the digest is unavailable. */
        std::string unavailable;
        bool operator==(const Subsystem&) const = default;
    };

    /** Always false until every simulation owner and external dependency has an
     * exact observation. Matching all available hashes cannot prove completeness. */
    bool complete = false;
    /** Fixed names/order, including explicit failure entries on closed/unbound scenes. */
    std::vector<Subsystem> subsystems;
    /** Owners/inputs omitted even when every subsystem digest is available.
     * scene_control visits actual scene control and camera-history values, but
     * several separately owned presentation systems remain public projections. */
    std::vector<std::string> unobserved;
    bool operator==(const SimulationObservation&) const = default;
};

} // namespace gdl::game
