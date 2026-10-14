#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::game {

class ClassDataSet;
class LevelFixtures;
class LevelWorld;
class LevelWorldCheckpoint;
struct PlayerRuntime;

/** Native Gold/Keys collection inside a caller-owned complete Stage transaction.
 * Uses the live collector/judge/mutation implementation. A refusal can follow
 * earlier collection; only the enclosing rewind can remove those mutations. */
class OrdinaryPickupPhase {
public:
    enum class Refusal { None, Binding, Domain, Party, Item, Lesson, Burst };
    struct Output {
        enum class Kind { Card, NamedSound, CharacterBark };
        Kind kind = Kind::Card;
        usize item = 0;  ///< exact placed-item lifetime index, not a resource ID
        s32 player = -1; ///< input slot; a theft bark names its original opener
        std::string name;
        bool operator==(const Output&) const = default;
    };
    struct Result {
        bool advanced = false;
        usize collected = 0; ///< includes partial key-ring takes
        std::vector<Output> outputs;
        Refusal refusal = Refusal::None;
        std::optional<usize> refusedItem;
    };

    /** Nonsecret, nontower native scene only: the challenge coin callback is
     * inactive in this domain. New help, active other pickup kinds, collection
     * bursts and unbound resources refuse; existing native no-op offers remain
     * harmless. Gold and Keys retain all inventory, pending gesture and chest
     * cleanup mutations. No HUD, audio, resource upload or external callback is
     * invoked. The returned ordered output is usable only after the WHOLE tick
     * commits, and its presentation/deduplication belongs to the caller. */
    static Result advance(std::span<PlayerRuntime> players, LevelWorld& world,
                          LevelFixtures& fixtures, const ClassDataSet& classes,
                          LevelWorldCheckpoint& checkpoint);
};

} // namespace gdl::game
