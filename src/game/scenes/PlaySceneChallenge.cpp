#include <algorithm>
#include <format>

#include "engine/core/Log.h"

#include "game/party/PlayerPowerups.h"
#include "game/players/ItemPickup.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {

std::optional<ChallengeHud::Look> PlayScene::hourglassLook() const {
    if (const auto stopTime = m_hud.hourglassLook(m_players)) {
        return stopTime;
    }
    if (m_sim.challenge.state() == SecretChallenge::State::Inactive) {
        return std::nullopt;
    }
    return m_challengeHud.look(m_sim.challenge.remaining(), m_sim.challenge.duration(),
                               !spawning() && !m_messages.active());
}

void PlayScene::beginChallenge() {
    if (!m_world->ref().isSecret()) {
        return;
    }
    PlayerPowerups::restrictToChallenge(m_players);
    if (m_context.levels == nullptr) {
        return;
    }
    s32 index = -1;
    for (const auto& realm : m_context.levels->realms()) {
        if (realm.id == LevelRef::kSecretRealm) {
            const auto found = std::ranges::find(realm.levels, m_world->ref().name);
            if (found != realm.levels.end()) {
                index = static_cast<s32>(found - realm.levels.begin());
            }
        }
    }
    std::vector<usize> coins;
    const auto& items = m_world->placedItems();
    for (usize i = 0; i < items.size(); ++i) {
        const auto& item = items.item(i);
        if (item.visible && !item.taken && item.subtype == static_cast<s32>(ItemKind::Gold)) {
            coins.push_back(i);
        }
    }
    const auto* level = m_world->level();
    constexpr u32 kTimedLevel = 4;
    if (level == nullptr || (level->flags & kTimedLevel) == 0 ||
        !m_sim.challenge.begin(index, level->timeLimit, coins)) {
        log::warn("Secret challenge {} has no time limit; refresh WDATA with gdlunpack",
                  m_world->ref().name);
        return;
    }
    m_challengeHud.bind(*m_device, m_world->powerups());
    m_hud.setCountTextures(&m_world->items().textures);
}

void PlayScene::collectChallengeCoin(usize item) {
    const usize before = m_sim.challenge.coinsLeft();
    const bool won = m_sim.challenge.collect(item);
    if (before == m_sim.challenge.coinsLeft()) {
        return;
    }
    const std::string icon =
        m_sim.challenge.rewardClass() == kSumnerClass
            ? "16_SUM"
            : std::format("16_{}COIN", classCode(m_sim.challenge.rewardClass()));
    for (auto& player : m_players) {
        if (player.life == PlayerLife::Standing) {
            m_hud.pickups().showCount(
                player.actor.player(), icon,
                static_cast<s32>(m_sim.challenge.totalCoins() - m_sim.challenge.coinsLeft()),
                static_cast<s32>(m_sim.challenge.totalCoins()));
            if (won) {
                player.actor.save().classUnlock |= m_sim.challenge.rewardMask();
            }
        }
    }
    if (!won) {
        return;
    }
    emitOutput(UnlockClassesOutput{m_sim.challenge.rewardMask()});
    openMessage("ALLCOINS", 0);
    m_audio.speakOverScroll("S_SECRETCHAR");
}

bool PlayScene::updateChallenge(f32 seconds) {
    if (m_world->ref().isSecret()) {
        PlayerPowerups::restrictToChallenge(m_players);
    }
    for (const s32 second : m_sim.challenge.step(seconds)) {
        if (second == 0) {
            m_audio.playNamed("S_SECRETCLOCKEN");
        } else {
            m_audio.playNamed(second % 2 == 0 ? "S_SECRETCLOCK1" : "S_SECRETCLOCK2");
        }
        constexpr s32 kTimeWarning = 8;
        constexpr s32 kCountdown = 5;
        if (second == kTimeWarning) {
            m_audio.narrate("S_TIMEISRUNNING", LevelSoundscape::Narrator::Primary);
        } else if (second > 0 && second <= kCountdown) {
            m_audio.narrate(std::format("S_COUNT{}", second), LevelSoundscape::Narrator::Primary);
        }
    }
    m_challengeHud.step(seconds);
    return m_sim.challenge.state() == SecretChallenge::State::Returning;
}

void PlayScene::suspendForChallenge() {
    // Travel is a confirmed application boundary, not a speculative tick. The
    // suspended scene's old history must not survive into its resumed epoch.
    endRecordedAudio();
    m_audio.suspend();
    // do_exit80077D38 calls the common warp cue even for the secret portal's
    // zero-tick departure. Start it after silencing the parent level, whose
    // sound banks remain alive while the challenge runs.
    if (m_sim.secretTravel) {
        m_audio.playNamed(PortalDeparture::kSound, PortalDeparture::kVolume);
    }
}

void PlayScene::resumeFromChallenge(std::span<const PartyMember> party) {
    usize standing = 0;
    for (auto& runtime : m_players) {
        const auto member = std::ranges::find(party, runtime.actor.player(), &PartyMember::player);
        if (member == party.end()) {
            continue;
        }
        runtime.actor.save() = member->save;
        runtime.slot = member->slot;
        runtime.helpHeard = member->helpHeard;
        if (member->resultsCheckpoint) {
            // The returned history includes the suspended stage's kills already.
            // Resume that same run without replacing its death/abort checkpoint.
            runtime.resultsCheckpoint = member->resultsCheckpoint;
            runtime.levelKills = 0;
        }
        if (runtime.life == PlayerLife::Standing) {
            runtime.actor.place(m_sim.secretReturnPosition +
                                Vec3{static_cast<f32>(standing++) * kSpawnSpacing, 0, 0});
            runtime.actor.settle(m_world->collision());
        }
    }
    m_audio.startMusic(m_context.assets,
                       m_world->level() != nullptr ? m_world->level()->musicVolume : 1.0f);
    m_sim.secretTravel = false;
    m_transition.cover();
    m_transition.clearAway();
}

} // namespace gdl::game
