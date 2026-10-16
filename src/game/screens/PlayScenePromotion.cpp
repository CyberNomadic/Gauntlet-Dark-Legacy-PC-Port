#include <algorithm>
#include <array>
#include <format>

#include "game/players/PickupVoices.h"
#include "game/screens/PlayScene.h"
#include "game/screens/PlayerPowerups.h"

namespace gdl::game {
bool PlayScene::ownsFigure(usize player, const PlayerFigure* figure) const {
    return player < m_players.size() &&
           (figure == nullptr || m_players[player].figure.get() == figure ||
            std::ranges::any_of(m_promotionFigures, [figure](const auto& retained) {
                return retained.get() == figure;
            }));
}

void PlayScene::activateFigure(usize player, PlayerFigure* figure) {
    auto& active = m_players[player].figure;
    if (active.get() == figure) {
        return;
    }
    if (figure == nullptr) {
        m_promotionFigures.push_back(std::move(active));
        return;
    }
    for (auto& retained : m_promotionFigures) {
        if (retained.get() == figure) {
            active.swap(retained);
            return;
        }
    }
}

void PlayScene::upgradeFigure(usize player) {
    auto& runtime = m_players[player];
    const auto& save = runtime.actor.save();
    const s32 level = save.progress().appearanceLevel();
    const auto cached = std::ranges::find_if(m_promotionBindings, [&](const auto& binding) {
        return binding.player == player && binding.character == save.character &&
               binding.color == save.color && binding.level == level;
    });
    if (cached != m_promotionBindings.end()) {
        // A discarded promotion keeps its resources alive: replay restores the
        // new costume's birth values rather than loading a second copy of it.
        activateFigure(player, cached->figure);
        runtime.figure->restoreLocal(cached->initial);
    } else {
        auto figure = PlayerFigure::load(*m_device, m_context.unpackedRoot, save, false);
        if (figure == nullptr) {
            return;
        }
        PromotionFigure binding;
        binding.player = player;
        binding.character = save.character;
        binding.color = save.color;
        binding.level = level;
        binding.figure = figure.get();
        figure->saveLocal(binding.initial);
        m_promotionBindings.push_back(std::move(binding));
        m_promotionFigures.push_back(std::move(runtime.figure));
        runtime.figure = std::move(figure);
    }
    runtime.figure->setCompanionPowerups(*m_device, m_world->powerups(), save.progress().inventory,
                                        &m_weapons);
}

s32 PlayScene::familiarTier(s32 player) const {
    for (const auto& runtime : m_players) {
        if (runtime.actor.player() == player && runtime.figure != nullptr) {
            return runtime.figure->familiarTier();
        }
    }
    return 0;
}
void PlayScene::updatePromotion(s32 ticks, f32 seconds) {
    // The spawn effect can finish before a class's START sequence. Hold the speech
    // as well as input until every standing body has settled into its stance.
    const bool entering = std::ranges::any_of(m_players, [](const PlayerRuntime& player) {
        return player.figure != nullptr && player.figure->animator().entering();
    });
    for (auto& player : m_players) {
        if (player.figure != nullptr) {
            player.figure->animate(0, ticks, seconds);
        }
    }
    m_promotion.animate(seconds);
    m_world->update(seconds, PlayerPowerups::timeStopped(m_players));
    m_sumner.update(seconds);
    m_effects.update(seconds);
    updateAmbience();
    if (entering) {
        return;
    }
    const auto* entry = m_promotion.current();
    if (entry == nullptr) {
        return;
    }
    auto& runtime = m_players[entry->player];
    auto& save = runtime.actor.save();
    const auto cue = m_promotion.update(ticks, m_promotionVoice.timing.playing());
    if (cue.voice) {
        LevelSoundscape::Speech name;
        if (PickupVoices::carriesPojo(save)) {
            name = m_audio.speechNarration(LevelSoundscape::kPojoName);
        } else if (runtime.figure != nullptr) {
            name = m_audio.speechFrom(runtime.figure->voice(),
                                      PickupVoices::nameOf(save.character, save.color));
        }
        m_promotionVoice = m_audio.speechPromotion(entry->voice, name);
    }
    if (cue.award) {
        save.progress().promotedLevel = entry->level;
        upgradeFigure(entry->player);
        EffectTrees::Setting setting;
        setting.seconds = 3;
        m_effects.startSet(*m_device, m_weapons, std::format("LEVELUP_{}", colorCode(save.color)),
                           runtime.actor.position(), setting);
    }
    if (cue.gem) {
        constexpr std::array<std::string_view, kPlayerCount> kGems{"GETGEMYELLOW", "GETGEMBLUE",
                                                                   "GETGEMRED", "GETGEMGREEN"};
        m_effects.start(*m_device, m_world->powerups(), kGems[static_cast<usize>(save.color)],
                        runtime.actor.position());
    }
}
} // namespace gdl::game
