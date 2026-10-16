#include "game/world/SumnerHints.h"

#include <algorithm>
#include <cstddef>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

#include "game/players/LevelRecord.h"
#include "game/players/Relics.h"
#include "game/world/LevelCatalog.h"
#include "game/world/TowerAccess.h"

namespace gdl::game {

void SumnerHints::appendState(NativeStateDigest& digest, const Simulation& state) const {
    const auto& [generalList, generalHint, guardian, legend, runestone] = state;
    digest.string("SumnerHints.native.1");
    digest.value(m_text);
    digest.values(generalList, generalHint);
    digest.fields<2>(guardian);
    digest.fields<2>(legend);
    digest.fields<2>(runestone);
}

bool SumnerHints::appendNativeDigest(NativeStateDigest& digest) const {
    appendState(digest, m_sim);
    return digest.good();
}

bool SumnerHints::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    const auto& [state, owner, binding] = saved;
    appendState(digest, state);
    return digest.good();
}

namespace {

constexpr std::string_view kGeneralList = "GENERAL_HINTS";
constexpr std::string_view kGuardianList = "BOSS_HINTS";
constexpr std::string_view kLegendList = "LEGEND_HINTS";
constexpr std::string_view kRunestoneList = "RUNE_HINTS";
constexpr std::string_view kGuardianTitles = "BOSSHINTDESC";
constexpr std::string_view kLegendTitles = "LEGENDHINTDESCS";
constexpr std::string_view kRunestoneTitles = "RUNEHINTDESCS";
constexpr s32 kLastGeneralList = 3;

u32 bitOf(s32 index) {
    return 1U << static_cast<u32>(index);
}

} // namespace

HintKnowledge HintKnowledge::ofParty(std::span<const ClassProgress> party) {
    HintKnowledge knowledge;
    const TowerAccess access(party);
    for (const s32 world : SumnerHints::kWorldOrder) {
        if (world > 0 && access.worldOpen(world)) {
            knowledge.worldsOpen |= bitOf(world);
        }
    }
    for (const ClassProgress& progress : party) {
        const Relics& relics = progress.relics;
        // A guardian is beaten when its realm's shard is held (given by the tower's order).
        for (s32 at = 1; at < SumnerHints::kGuardianCount; ++at) {
            const s32 world = SumnerHints::kWorldOrder[static_cast<usize>(at)];
            if (relics.hasShard(LevelRef::orderOf(world))) {
                knowledge.guardiansBeaten |= bitOf(world);
            }
        }
        knowledge.legendsFound |= relics.legends;
        knowledge.runestonesFound |= relics.runes;
        // A wing's icons: a count marked complete, or the whole of what the statue wants
        // (towerAllPlayersMetLevelReq).
        for (usize kind = 0; kind < knowledge.wingsOpen.size(); ++kind) {
            const s32 pieces = relics.gargoylePieces[kind];
            knowledge.wingsOpen[kind] = knowledge.wingsOpen[kind] || pieces < 0 ||
                                        relics.gargoyleComplete(static_cast<s32>(kind));
        }
        for (usize pass = 0; pass < LevelRecord::kPasses; ++pass) {
            knowledge.guardianTries[pass] |= progress.levels.bossDeaths[pass];
            knowledge.legendTries[pass] |= progress.levels.legendLevels[pass];
            knowledge.runestoneTries[pass] |= progress.levels.runeLevels[pass];
        }
    }
    return knowledge;
}

bool SumnerHints::load(const std::filesystem::path& file) {
    m_localBinding = std::make_shared<const u8>(0);
    m_sim.generalList = -1;
    m_sim.generalHint = -1;
    beginVisit();
    return m_text.load(file);
}

void SumnerHints::beginVisit() {
    m_sim.guardian = Cursor{0, 0};
    m_sim.legend = Cursor{0, 0};
    m_sim.runestone = Cursor{-1, 0};
}

/** The next hint of the list for the furthest wing open, then of the wings before it, and
 * round again. */
void SumnerHints::stepGeneral(const HintKnowledge& knowledge) {
    const MessageList* lists = m_text.findList(kGeneralList);
    for (s32 tries = 0; tries < 2 * (kLastGeneralList + 2); ++tries) {
        if (m_sim.generalList < 0) {
            m_sim.generalList = kLastGeneralList;
        }
        while (m_sim.generalList > 0 && !knowledge.wingsOpen[static_cast<usize>(m_sim.generalList - 1)]) {
            m_sim.generalHint = -1;
            --m_sim.generalList;
        }
        ++m_sim.generalHint;
        const MessageInfo* message =
            lists != nullptr ? m_text.listed(*lists, static_cast<usize>(m_sim.generalList)) : nullptr;
        const auto count = message != nullptr ? static_cast<s32>(message->pages.size()) : 0;
        if (m_sim.generalHint < count) {
            return;
        }
        m_sim.generalHint = -1;
        --m_sim.generalList;
    }
}

/** The next open world not yet `done`; with none left, the next open one at all; with none
 * open, the first. */
void SumnerHints::stepWorlds(Cursor& cursor, s32 count, u32 open, u32 done) {
    s32 from = cursor.index + 1;
    for (;;) {
        if (from >= count) {
            from %= count;
            cursor.widened = 1;
        }
        from = std::max(from, 1);
        s32 at = from;
        for (; at < count; ++at) {
            const u32 world = bitOf(kWorldOrder[static_cast<usize>(at)]);
            if ((open & world) != 0 && (cursor.widened != 0 || (done & world) == 0)) {
                break;
            }
        }
        if (at != count) {
            cursor.index = at;
            return;
        }
        if (cursor.widened > 1) {
            cursor.widened = 1;
            cursor.index = 1;
            return;
        }
        ++cursor.widened;
        from = 1;
    }
}

/** The next runestone still missing; with them all found, the next one at all. */
void SumnerHints::stepRunestones(Cursor& cursor, u32 found) {
    const auto count = static_cast<s32>(kRunestoneOrder.size());
    s32 from = cursor.index + 1;
    for (;;) {
        if (from >= count) {
            from %= count;
            cursor.widened = 1;
        }
        s32 at = from;
        for (; at < count; ++at) {
            const u32 stone = bitOf(kRunestoneOrder[static_cast<usize>(at)] - 1);
            if (cursor.widened != 0 || (found & stone) == 0) {
                break;
            }
        }
        if (at != count) {
            cursor.index = at;
            return;
        }
        if (cursor.widened > 1) {
            cursor.widened = 1;
            cursor.index = 0;
            return;
        }
        ++cursor.widened;
        from = 1;
    }
}

/** One passage, and one more for each time the party has tried. */
s32 SumnerHints::passagesFor(u32 bit, const std::array<u32, 2>& tries) {
    if ((tries[1] & bit) != 0) {
        return 3;
    }
    return (tries[0] & bit) != 0 ? 2 : 1;
}

HintPage SumnerHints::pageOf(std::string_view titles, usize title, std::string_view list,
                             usize entry, s32 passages, s32 gap) const {
    HintPage page;
    page.gap = gap;
    if (const auto found = m_text.find(titles); found.has_value()) {
        const MessageInfo& names = m_text.message(*found);
        if (title < names.pages.size()) {
            page.title = names.pages[title];
        }
    }
    const MessageList* entries = m_text.findList(list);
    const MessageInfo* message = entries != nullptr ? m_text.listed(*entries, entry) : nullptr;
    if (message != nullptr) {
        page.scale = message->scale;
        const usize count = std::min(static_cast<usize>(passages), message->pages.size());
        page.passages.assign(message->pages.begin(),
                             message->pages.begin() + static_cast<std::ptrdiff_t>(count));
    }
    return page;
}

HintPage SumnerHints::next(HintTopic topic, const HintKnowledge& knowledge,
                           std::string_view generalTitle) {
    if (!loaded()) {
        return {};
    }
    switch (topic) {
    case HintTopic::General: {
        stepGeneral(knowledge);
        HintPage page;
        page.title = std::string(generalTitle);
        page.centred = true;
        const MessageList* lists = m_text.findList(kGeneralList);
        const MessageInfo* message = lists != nullptr && m_sim.generalList >= 0
                                         ? m_text.listed(*lists, static_cast<usize>(m_sim.generalList))
                                         : nullptr;
        if (message != nullptr && m_sim.generalHint >= 0 &&
            static_cast<usize>(m_sim.generalHint) < message->pages.size()) {
            page.scale = message->scale;
            page.passages.push_back(message->pages[static_cast<usize>(m_sim.generalHint)]);
        }
        return page;
    }
    case HintTopic::Guardians: {
        stepWorlds(m_sim.guardian, kGuardianCount, knowledge.worldsOpen, knowledge.guardiansBeaten);
        const auto entry = static_cast<usize>(m_sim.guardian.index);
        return pageOf(kGuardianTitles, entry, kGuardianList, entry,
                      passagesFor(bitOf(kWorldOrder[entry]), knowledge.guardianTries), kPassageGap);
    }
    case HintTopic::Legends: {
        stepWorlds(m_sim.legend, kLegendCount, knowledge.worldsOpen, knowledge.legendsFound);
        const auto entry = static_cast<usize>(m_sim.legend.index);
        return pageOf(kLegendTitles, entry, kLegendList, entry,
                      passagesFor(bitOf(kWorldOrder[entry]), knowledge.legendTries), kPassageGap);
    }
    case HintTopic::Runestones: {
        stepRunestones(m_sim.runestone, knowledge.runestonesFound);
        const auto stone =
            static_cast<usize>(kRunestoneOrder[static_cast<usize>(m_sim.runestone.index)] - 1);
        return pageOf(kRunestoneTitles, stone, kRunestoneList, stone,
                      passagesFor(bitOf(static_cast<s32>(stone)), knowledge.runestoneTries), 0);
    }
    }
    return {};
}

void SumnerHints::saveLocal(LocalSnapshot& saved) const {
    saved.state = m_sim;
    saved.owner = this;
    saved.binding = m_localBinding;
}

bool SumnerHints::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.owner == this && saved.binding.lock() == m_localBinding;
}

bool SumnerHints::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.state;
    return true;
}

} // namespace gdl::game
