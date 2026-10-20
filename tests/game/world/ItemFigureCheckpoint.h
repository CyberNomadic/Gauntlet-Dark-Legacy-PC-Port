#pragma once

#include "game/world/ItemFigure.h"

namespace gdl::game {

/** Bound native CPU figures and genuinely unbound fixture placeholders share
 * this exact value owner. No tree/resource lookup or GPU upload is permitted. */
class ItemFigureCheckpoint {
public:
    struct State {
        ItemFigure::Checkpoint state;
        bool bound = false;
        bool archive = false;
        bool valid() const;
        bool operator==(const State&) const = default;
    };
    /** Failed rebinds retaining a previous model/pose are explicitly refused.
     * Never-bound or initially missing figures retain every scalar/clock. */
    static std::optional<State> capture(const ItemFigure& figure);
    /** Borrows only this already-bound prototype's live immutable archives. */
    static std::unique_ptr<ItemFigure> prepare(const ItemFigure& figure, const State& saved);
};
} // namespace gdl::game
