#pragma once

#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "game/netplay/EffectIdentity.h"
#include "game/screens/TurboEffectResources.h"

namespace gdl::game {
/** Private visual replay of certified native B/C emissions. This owns no combat
 * callbacks, actors, sound players or render device. The resource catalog and its
 * archives must outlive clear(); all starts use their already-bound prototypes. */
class TurboEffectSimulation {
public:
    static constexpr usize kMaxEffects = 512;
    static constexpr usize kMaxEmissions = 2048;

    struct Pose {
        Mat4 body{1};
        Mat4 root{1};
        /** Every requested name must be sampled. Explicit nullopt is native's
         * missing-node fallback to the body; an absent map key is unsampled. */
        std::map<std::string, std::optional<Mat4>> nodes;
    };
    struct Entry {
        EffectIdentity origin;
        u32 resource = 0;
        u32 id = 0; ///< local pool handle only, never a reconciliation identity
    };
    struct State {
        Entry entry;
        EffectTrees::Checkpoint effect;
        std::optional<TurboEmission::Attachment> attachment;
        std::optional<TurboEmission::Particle> particle;
        std::optional<u32> particleSeed;
    };
    struct Bank {
        u32 resource = 0; ///< representative of this shared archive/lender bank
        TextureAnimator::Checkpoint clock;
    };
    struct Checkpoint {
        std::vector<State> effects;
        std::vector<Bank> banks;
        std::vector<EffectIdentity> emitted;
    };

    void bind(const TurboEffectResources& resources);
    void clear();
    /** base.effectOrdinal must be zero. Seeds use complete native chain order,
     * including invisible entries; standalone effects reject absent birth seeds.
     * A repeated complete strike is idempotent, including after its effects die.
     * Failed validation changes nothing; a failed native start clears this replay. */
    bool start(const EffectIdentity& base, const TurboEmission::Strike& strike, const Pose& pose,
               std::span<const std::optional<u32>> particleSeeds = {});
    /** Placement precedes the shared native effect update, as in PlayerAttacks.
     * No guessed particle age or scalar lifetime is advanced outside EffectTrees. */
    bool step(f32 seconds, const Pose& pose);
    Checkpoint checkpoint() const;
    /** Transactional restore against the same trusted catalog, including shared
     * texture clocks for banks whose final live effect has already disappeared. */
    bool restore(const Checkpoint& state);
    std::span<const Entry> entries() const { return m_entries; }
    const std::set<EffectIdentity>& emitted() const { return m_emitted; }
    const EffectTrees& effects() const { return m_effects; }

private:
    struct Binding {
        std::optional<TurboEmission::Attachment> attachment;
        std::optional<TurboEmission::Particle> particle;
        std::optional<u32> particleSeed;
    };
    static EffectTrees::Prepared prepared(const TurboEffectResources::Resource& resource);
    static std::optional<Mat4> placement(const Binding& binding, const Pose& pose);
    static bool validPose(const Pose& pose);
    bool rememberBank(u32 resource);
    const TurboEffectResources* m_resources = nullptr;
    EffectTrees m_effects;
    std::vector<Entry> m_entries;
    std::map<u32, Binding> m_bindings;
    std::vector<u32> m_banks;
    std::set<EffectIdentity> m_emitted;
};
} // namespace gdl::game
