#pragma once

#include <map>
#include <tuple>

#include "engine/core/Types.h"

#include "game/netplay/AudioSync.h"
#include "game/netplay/CombatSnapshot.h"
#include "game/world/LevelSoundscape.h"

namespace gdl::game {
struct MatchContext;
/** Resolves commands against a locally discovered native bank roster. Network
 * text is only a lookup key; it is never used to construct a filesystem path. */
class ReplicaAudio {
public:
    ReplicaAudio() = default;
    ~ReplicaAudio();
    ReplicaAudio(const ReplicaAudio&) = delete;
    ReplicaAudio& operator=(const ReplicaAudio&) = delete;
    ReplicaAudio(ReplicaAudio&&) = delete;
    ReplicaAudio& operator=(ReplicaAudio&&) = delete;
    void open(const std::filesystem::path& root, SoundPlayer* output, const LevelAudioInfo* info,
              f32 musicVolume);
    void clear();
    bool consume(AudioSync& stream);
    void music(const CombatSnapshot& state);
    usize voices() const { return m_handles.size(); }
    enum class Prediction : u8 { Played, Duplicate, Unavailable, Invalid, Capacity };
    static constexpr u64 kPredictionTicks = 120;
    static constexpr usize kPredictionCapacity = 2 * kPredictionTicks * InputCommand::kSeats;
    /** Freeze the admitted local seats. Changing epoch/grant stops their predicted
     * voices before forgetting their keys; banks remain owned here until clear(). */
    bool beginPrediction(const MatchContext& context, u8 peer);
    /** Loading-time preparation, using the native modulo-eight voice dispatch.
     * Decoded clips stay owned here, never borrowed from a rendered figure. */
    bool prepareWeapon(u8 seat, s32 character);
    /** A successful native release only, never a requested button or damage event.
     * One ordinal-zero cue represents the whole native one/three/five-shot fan. */
    Prediction predict(const ShotIdentity& origin, u8 shots, u32 weaponFlags = 0);
    /** Complete current replay roster, including launches that immediately hit a
     * wall. Absence cancels only unresolved, still-future speculative sounds:
     * a resolved tick alone does not prove its reliable host audio was rejected. */
    bool reconcile(const CombatSnapshot& state, std::span<const ShotIdentity> pending);

private:
    struct Bank {
        std::filesystem::path path;
        std::unique_ptr<SoundSet> sounds;
    };
    using CueKey = std::tuple<ShotIdentity, std::string, u32>;
    struct Heard {
        SoundHandle local = kNoSound;
        bool confirmed = false;
    };
    struct Prepared {
        std::string bank;
        u32 call = 0;
        std::optional<SoundSequence> sequence;
    };
    SoundSet* bank(std::string_view name);
    static std::optional<SoundSequence> prepare(SoundSet& sounds, u32 call);
    void clearPrediction();
    void stopPrediction(u8 seat);
    bool owned(const ShotIdentity& origin) const;
    bool prepared(u8 seat, const SoundPlayer::Event& event) const;
    SoundPlayer* m_output = nullptr;
    std::unique_ptr<AssetLocator> m_assets;
    LevelSoundscape m_music;
    f32 m_musicVolume = 1;
    std::map<std::string, Bank> m_banks;
    std::map<SoundHandle, SoundHandle> m_handles;
    std::array<std::optional<Prepared>, InputCommand::kSeats> m_throws;
    std::optional<Prepared> m_fan;
    std::array<std::optional<Prepared>, 4> m_elements;
    std::map<CueKey, Heard> m_heard;
    std::array<u32, InputCommand::kSeats> m_grants{};
    std::array<u32, InputCommand::kSeats> m_rosterGrants{};
    std::array<u64, InputCommand::kSeats> m_retired{};
    u64 m_epoch = 0;
    u64 m_resolved = 0;
};
} // namespace gdl::game
