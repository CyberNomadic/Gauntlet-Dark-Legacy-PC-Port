#include "game/screens/ReplicaAudio.h"

#include <exception>
#include <format>
#include <set>

#include "engine/core/Log.h"
#include "engine/core/Strings.h"

#include "game/netplay/MatchSession.h"
#include "game/players/ClassData.h"
#include "game/world/PlayerMissiles.h"

namespace gdl::game {
ReplicaAudio::~ReplicaAudio() {
    clear();
}
void ReplicaAudio::clear() {
    m_music.close();
    clearPrediction();
    if (m_output != nullptr) {
        for (const auto& [remote, local] : m_handles) {
            m_output->stop(local);
        }
    }
    m_handles.clear();
    m_fan.reset();
    m_elements = {};
    m_banks.clear();
    m_assets.reset();
    m_output = nullptr;
}
void ReplicaAudio::open(const std::filesystem::path& root, SoundPlayer* output,
                        const LevelAudioInfo* info, f32 musicVolume) {
    clear();
    m_output = output;
    m_assets = std::make_unique<AssetLocator>(root);
    m_musicVolume = musicVolume;
    if (m_output == nullptr) {
        return;
    }
    if (const auto directory = m_assets->find("AUDIO")) {
        for (const auto& entry : std::filesystem::directory_iterator(*directory)) {
            if (entry.is_regular_file() &&
                toLowerAscii(entry.path().extension().string()) == ".vbk") {
                m_banks.try_emplace(normalizeAssetName(entry.path().stem().string()),
                                    Bank{entry.path(), nullptr});
            }
        }
    }
    m_music.open(root, output, info);
    // Match LevelSoundscape::bankOf: the first named match owns this cue even
    // when its sequence is broken. Never fall through to a different recording.
    const auto named = [&](std::string_view cue) -> std::optional<Prepared> {
        for (const auto& name : {info != nullptr ? info->bank : std::string{},
                                 std::string{"COMMON"}, std::string{"TOWAMB"}}) {
            if (auto* sounds = bank(normalizeAssetName(name))) {
                if (const auto call = sounds->find(cue)) {
                    return Prepared{normalizeAssetName(name), *call, prepare(*sounds, *call)};
                }
            }
        }
        return std::nullopt;
    };
    m_fan = named("S_SUPERSHOT");
    // PlayerArsenal::launchWeapon uses this low-nibble order; spread overrides
    // the elemental cue. Predecode every possibility before gameplay begins.
    constexpr std::array<std::string_view, 4> elements{"S_AMULETFIRE", "S_AMULETLIGHTNI",
                                                       "S_AMULETLIGHT", "S_AMULETACID"};
    for (usize index = 0; index < elements.size(); ++index) {
        m_elements[index] = named(elements[index]);
    }
}
SoundSet* ReplicaAudio::bank(std::string_view name) {
    const auto found = m_banks.find(std::string{name});
    if (found == m_banks.end()) {
        return nullptr;
    }
    try {
        if (!found->second.sounds) {
            auto sounds = std::make_unique<SoundSet>();
            if (!sounds->load(found->second.path)) {
                return nullptr;
            }
            found->second.sounds = std::move(sounds);
        }
        return found->second.sounds.get();
    } catch (const std::exception& error) {
        log::warn("Guest audio bank {}: {}", name, error.what());
        return nullptr;
    }
}
std::optional<SoundSequence> ReplicaAudio::prepare(SoundSet& sounds, u32 call) {
    try {
        auto sequence = sounds.sequence(call);
        if (sequence.bank.empty() || sequence.steps.empty() || sequence.loops() ||
            std::ranges::any_of(sequence.steps, [](const auto& step) {
                return step.clip == nullptr || step.clip->channels == 0 ||
                       step.clip->sampleRate == 0 || step.clip->frames() == 0;
            })) {
            return std::nullopt;
        }
        return sequence;
    } catch (const std::exception& error) {
        log::warn("Guest throw audio call {}: {}", call, error.what());
        return std::nullopt;
    }
}
void ReplicaAudio::stopPrediction(u8 seat) {
    std::erase_if(m_heard, [&](const auto& item) {
        if (std::get<0>(item.first).seat != seat) {
            return false;
        }
        if (m_output != nullptr) {
            m_output->stop(item.second.local);
        }
        std::erase_if(m_handles,
                      [&](const auto& handle) { return handle.second == item.second.local; });
        return true;
    });
    m_throws[seat].reset();
    m_retired[seat] = 0;
}
void ReplicaAudio::clearPrediction() {
    for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
        stopPrediction(seat);
    }
    m_grants = {};
    m_rosterGrants = {};
    m_epoch = 0;
    m_resolved = 0;
}
bool ReplicaAudio::beginPrediction(const MatchContext& context, u8 peer) {
    if (!context.valid() || peer < 2 || peer > InputCommand::kSeats) {
        clearPrediction();
        return false;
    }
    if (context.epoch != m_epoch) {
        clearPrediction();
        m_epoch = context.epoch;
    }
    m_rosterGrants = context.grants;
    for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
        const u32 grant = context.owners[seat] == peer ? context.grants[seat] : 0;
        if (grant != m_grants[seat]) {
            stopPrediction(seat);
            m_grants[seat] = grant;
        }
    }
    return true;
}
bool ReplicaAudio::prepareWeapon(u8 seat, s32 character) {
    if (seat >= InputCommand::kSeats || m_grants[seat] == 0 || character < 0 ||
        character >= kClassCount || m_output == nullptr) {
        return false;
    }
    m_throws[seat].reset();
    // PlayerFigure::loadMissile uses the shadowed base-class bank for secret
    // classes too. Sumner (16) therefore follows the same native modulo rule.
    const auto voice = classCode(character % kStartingClassCount);
    if (auto* sounds = bank(voice)) {
        if (const auto call = sounds->find(std::format("S_{}THROW", voice))) {
            m_throws[seat] = Prepared{std::string{voice}, *call, prepare(*sounds, *call)};
        }
    }
    return m_throws[seat] && m_throws[seat]->sequence;
}
bool ReplicaAudio::owned(const ShotIdentity& origin) const {
    return origin.valid() && origin.ordinal == 0 && origin.epoch == m_epoch &&
           m_grants[origin.seat] != 0 && m_grants[origin.seat] == origin.grant;
}
bool ReplicaAudio::prepared(u8 seat, const SoundPlayer::Event& event) const {
    if (event.kind != SoundPlayer::Event::Kind::Play || event.after != kNoSound ||
        event.category != SoundCategory::Effects) {
        return false;
    }
    const auto matches = [&](const auto& cue) {
        return cue && cue->bank == event.bank && cue->call == event.call;
    };
    return matches(m_throws[seat]) || matches(m_fan) || std::ranges::any_of(m_elements, matches);
}
ReplicaAudio::Prediction ReplicaAudio::predict(const ShotIdentity& origin, u8 shots,
                                               u32 weaponFlags) {
    if (!owned(origin) || (shots != 1 && shots != 3 && shots != 5) ||
        !PlayerMissiles::replayModifiers(weaponFlags) ||
        (origin.tick > m_resolved && origin.tick - m_resolved > kPredictionTicks)) {
        return Prediction::Invalid;
    }
    if (origin.tick <= m_resolved || origin.tick <= m_retired[origin.seat]) {
        return Prediction::Duplicate;
    }
    const u32 element = weaponFlags & 0xFU;
    const auto* selected = &m_throws[origin.seat];
    if (shots != 1 || (weaponFlags & (powerup::kThreeWayShot | powerup::kFiveWayShot)) != 0) {
        selected = &m_fan;
    } else if (element != 0) {
        selected = &m_elements[element - 1];
    }
    const auto& preparedCue = *selected;
    if (m_output == nullptr || !preparedCue || !preparedCue->sequence) {
        return Prediction::Unavailable;
    }
    const auto& sequence = *preparedCue->sequence;
    const CueKey key{origin, sequence.bank, sequence.call};
    if (m_heard.contains(key) || std::ranges::any_of(m_heard, [&](const auto& heard) {
            return std::get<0>(heard.first) == origin && heard.second.confirmed;
        })) {
        return Prediction::Duplicate;
    }
    if (m_heard.size() >= kPredictionCapacity) {
        return Prediction::Capacity;
    }
    for (auto& [earlier, heard] : m_heard) {
        if (std::get<0>(earlier) == origin && earlier != key && !heard.confirmed &&
            m_output->isPlaying(heard.local)) {
            m_output->stop(heard.local);
        }
    }
    const auto local =
        m_output->play(sequence, LevelSoundscape::kStepVolume, SoundCategory::Effects);
    if (local == kNoSound) {
        return Prediction::Unavailable;
    }
    m_heard.emplace(key, Heard{local, false});
    return Prediction::Played;
}
bool ReplicaAudio::reconcile(const CombatSnapshot& state, std::span<const ShotIdentity> pending) {
    if (state.motion.epoch != m_epoch || state.motion.tick < m_resolved ||
        pending.size() > kPredictionCapacity ||
        !std::ranges::all_of(pending, [&](const auto& origin) { return owned(origin); })) {
        return false;
    }
    m_resolved = state.motion.tick;
    const std::set<ShotIdentity> live(pending.begin(), pending.end());
    const u64 retired = m_resolved > kPredictionTicks ? m_resolved - kPredictionTicks : 0;
    for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
        m_retired[seat] = std::max(m_retired[seat], retired);
    }
    std::erase_if(m_heard, [&](auto& item) {
        const auto& origin = std::get<0>(item.first);
        auto& heard = item.second;
        if (!heard.confirmed && origin.tick > m_resolved && !live.contains(origin) &&
            m_output != nullptr && m_output->isPlaying(heard.local)) {
            m_output->stop(heard.local);
        }
        // Visual authority can overtake reliable audio. Unconfirmed heard keys
        // must survive that gap, even after a corrected-out voice has finished.
        return heard.confirmed && origin.tick <= retired &&
               (m_output == nullptr || !m_output->isPlaying(heard.local));
    });
    return true;
}
bool ReplicaAudio::consume(AudioSync& stream) {
    if (m_output != nullptr) {
        std::erase_if(m_handles,
                      [this](const auto& value) { return !m_output->isPlaying(value.second); });
    }
    while (const auto cue = stream.take()) {
        if (m_output == nullptr) {
            continue; // Audio-disabled/headless clients still consume the stream.
        }
        const auto* event = &cue->event;
        if (cue->origin && m_epoch != 0 &&
            (cue->origin->epoch != m_epoch ||
             cue->origin->grant != m_rosterGrants[cue->origin->seat])) {
            continue; // An old occupant must not sound for a newly granted seat.
        }
        using Kind = SoundPlayer::Event::Kind;
        if (event->kind == Kind::Play) {
            if (!m_banks.contains(event->bank) || m_handles.contains(event->handle) ||
                m_handles.size() >= 1024) {
                return false;
            }
            bool attributed = cue->origin && owned(*cue->origin);
            if (attributed && !prepared(cue->origin->seat, *event)) {
                const auto& normal = m_throws[cue->origin->seat];
                if ((normal && normal->bank == event->bank) ||
                    (m_fan && m_fan->bank == event->bank) || (normal && m_fan) ||
                    std::ranges::any_of(m_elements, [&](const auto& element) {
                        return element && element->bank == event->bank;
                    })) {
                    return false; // A known bank/call cannot confirm another cue.
                }
                // Missing optional preparation must not disconnect a client.
                // Ordinary authority playback still validates/loads its bank.
                attributed = false;
            }
            if (attributed) {
                const CueKey key{*cue->origin, event->bank, event->call};
                if (const auto heard = m_heard.find(key); heard != m_heard.end()) {
                    heard->second.confirmed = true;
                    // A completed predicted sound still counts as heard. Never
                    // restart it merely because its authoritative cue was late.
                    if (m_output->isPlaying(heard->second.local)) {
                        m_handles.emplace(event->handle, heard->second.local);
                        m_output->setVolume(heard->second.local, event->value);
                    }
                    continue;
                }
                if (m_heard.size() >= kPredictionCapacity) {
                    // Preserve authority even when unresolved predictions fill
                    // the bounded ledger. Fence only later local prediction;
                    // never use visual age to suppress a never-heard host cue.
                    auto& retired = m_retired[cue->origin->seat];
                    retired = std::max(retired, cue->origin->tick);
                    attributed = false;
                }
                // A corrected modifier can change the native call at this exact
                // release. Stop its wrong speculative cue, but retain that key
                // so an older replay cannot sound it again.
                for (auto& [earlier, heard] : m_heard) {
                    if (std::get<0>(earlier) == *cue->origin && earlier != key &&
                        !heard.confirmed) {
                        if (m_output->isPlaying(heard.local)) {
                            m_output->stop(heard.local);
                        }
                        heard.confirmed = true; // this exact release chose the other cue
                    }
                }
            }
            try {
                auto* sounds = bank(event->bank);
                if (sounds == nullptr || event->call >= sounds->size()) {
                    return false;
                }
                const auto previous = m_handles.find(event->after);
                const auto handle = m_output->playAfter(
                    previous != m_handles.end() ? previous->second : kNoSound,
                    sounds->sequence(event->call), event->value, event->category);
                if (handle != kNoSound) {
                    m_handles.emplace(event->handle, handle);
                    if (attributed) {
                        m_heard.emplace(CueKey{*cue->origin, event->bank, event->call},
                                        Heard{handle, true});
                    }
                }
            } catch (const std::exception& error) {
                log::error("Guest audio {} call {}: {}", event->bank, event->call, error.what());
                return false;
            }
        } else if (const auto handle = m_handles.find(event->handle); handle != m_handles.end()) {
            switch (event->kind) {
            case Kind::Stop:
                m_output->stop(handle->second);
                m_handles.erase(handle);
                break;
            case Kind::Volume: m_output->setVolume(handle->second, event->value); break;
            case Kind::Pan: m_output->setPan(handle->second, event->value); break;
            case Kind::Play: break;
            }
        }
    }
    return true;
}
void ReplicaAudio::music(const CombatSnapshot& state) {
    m_music.followMusic(m_assets.get(), state.musicArea, static_cast<s32>(state.musicLevel),
                        m_musicVolume);
}
} // namespace gdl::game
