#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

#include "game/characters/LegendsProgress.h"

namespace gdl::game {

struct LegendsStageUnlock {
    std::string map;
    u32 realm = 0;
    u32 stage = 0;
    u64 requiredObelisks = 0;
    std::vector<std::string> preceding;
};

/** N64 Tower portal admission, not developer-console travel permission. The
 * executable's level-table order is significant: Castle's second stage is
 * castle6 and Ice's boss entry precedes ice6. Unknown destinations stay locked. */
class LegendsN64Unlocks {
public:
    static LegendsN64Unlocks decode(std::span<const u8> program);
    std::span<const LegendsStageUnlock> stages() const { return m_stages; }
    bool isTower(std::string_view map) const;
    bool allows(std::string_view map, const LegendsProgress& party) const;

private:
    std::vector<LegendsStageUnlock> m_stages;
};

} // namespace gdl::game
