#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "engine/io/NativeStateDigest.h"

#include "game/enemies/Enemies.h"

namespace gdl::game {

/** The ordered, resolved placement records waiting for the native watch phase.
 * The initial roster stays immutable. Native rewind restores consumed records
 * only within the owner and level binding that captured them. */
class PlacedEnemies {
public:
    static constexpr f32 kWatchReach = 50;

    struct Placement {
        EnemySpawn spawn;
        s32 kind = -1;
        f32 facing = 0;
        f32 viewRadius = 0;
        f32 sight = 0;
        std::optional<usize> carried;
        bool operator==(const Placement& /*other*/) const;
        friend void visitNativeState(NativeStateDigest& digest, const Placement& value) {
            digest.fields<6>(value);
        }
    };

    PlacedEnemies() = default;
    PlacedEnemies(const PlacedEnemies&) = delete;
    PlacedEnemies& operator=(const PlacedEnemies&) = delete;
    PlacedEnemies(PlacedEnemies&&) = delete;
    PlacedEnemies& operator=(PlacedEnemies&&) = delete;
    ~PlacedEnemies() = default;

    /** Load-time records have already passed the level's native resource and
     * party filters. */
    void bind(std::vector<Placement> roster);
    void clear();
    void observe(std::optional<ViewVolume> view, const Vec3& attention);
    bool inView(const Vec3& position, f32 radius) const;
    usize count() const { return m_sim.pending.size(); }
    std::span<const Placement> roster() const { return m_roster; }
    std::span<const usize> pending() const { return m_sim.pending; }

    /** Read-only preflight in native order, with the same priority override as
     * activate. A private owner can reject an unsupported birth before mutation. */
    std::vector<Placement> activations() const;
    /** Calls the native spawn dispatcher synchronously, then consumes each seen
     * record even when the spawn failed. No-view loading only uses free slots;
     * watched placements retain their authored replacement priority. */
    void activate(const std::function<void(const Placement&)>& stand);

private:
    std::optional<Placement> activation(usize index) const;
    struct Simulation {
        std::vector<usize> pending;
        std::optional<ViewVolume> view;
        Vec3 attention{0};
        friend void visitNativeState(NativeStateDigest& digest, const Simulation& state) {
            const auto& [pending, view, attention] = state;
            digest.value(pending);
            digest.flag(view.has_value());
            if (view) {
                digest.fields<8>(*view);
            }
            digest.value(attention);
        }
    };

public:
    /** Same-owner value rewind; the loaded placement roster stays bound. */
    class LocalSnapshot {
        friend class PlacedEnemies;
        friend class PlacedEnemiesCheckpoint; // Test-only legacy diagnostics.
        Simulation m_values;
        std::weak_ptr<const u8> m_owner;
        u64 m_epoch = 0;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool equalLocal(const LocalSnapshot& a, const LocalSnapshot& b) const;
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

private:
    bool appendStateDigest(NativeStateDigest& digest, const Simulation& state) const;
    friend class PlacedEnemiesCheckpoint; // No diagnostic records enter runtime.
    Simulation m_sim;
    u64 m_localEpoch = 1;
    std::vector<Placement> m_roster;
    bool m_bound = false;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};

} // namespace gdl::game
