#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"

namespace gdl {

/** A tree found in one of a scope's archives. */
struct TreeHit {
    ItemArchive* archive = nullptr;
    u32 index = 0;
};

/** An object found in one of a scope's archives, by the name that matched. */
struct ObjectHit {
    ItemArchive* archive = nullptr;
    u32 index = 0;
    std::string name;
};

/**
 * The archives an item may take its art from, in the order the original searches its
 * headers: the level's own items, the realm's, then whatever else the caller lends (enemy
 * archives, powerups, weapons). A tree is matched by exact name across them; an object
 * is matched by its name, then the name with L1, then with L1ROOT appended, the way the
 * original's item setup retries, each spelling tried across every archive before the next.
 * The scope borrows its archives; they must outlive it.
 */
class AssetScope {
public:
    static constexpr std::array<std::string_view, 3> kObjectSuffixes{"", "L1", "L1ROOT"};

    AssetScope() = default;
    explicit AssetScope(ItemArchive& only) { add(only); }
    /** The loaded archives of `archives`, in their order; nulls are skipped. */
    explicit AssetScope(std::span<ItemArchive* const> archives) {
        for (ItemArchive* archive : archives) {
            if (archive != nullptr) {
                add(*archive);
            }
        }
    }
    AssetScope(ItemArchive& first, ItemArchive* second) {
        add(first);
        if (second != nullptr) {
            add(*second);
        }
    }

    /** Appends an archive to the end of the search order; an empty one is ignored. */
    void add(ItemArchive& archive);
    std::span<ItemArchive* const> archives() const { return m_archives; }
    bool empty() const { return m_archives.empty(); }
    /** The archive searched first, or nullptr when there is none. */
    ItemArchive* primary() const { return m_archives.empty() ? nullptr : m_archives.front(); }

    /** The first archive holding an animation tree named `name`. */
    std::optional<TreeHit> findTree(std::string_view name) const;
    /** The first archive holding an object named `name`, or that name with an authored
     * suffix, each spelling tried across every archive before the next. */
    std::optional<ObjectHit> findObject(std::string_view name) const;
    /** The archive holding the tree `name`, else the primary one; nullptr for an empty scope. */
    ItemArchive* archiveForTree(std::string_view name) const;

    /** Every archive's textures, in search order, for binding external texture slots. */
    std::vector<TextureSet*> textureLenders() const;

private:
    std::vector<ItemArchive*> m_archives;
};

} // namespace gdl
