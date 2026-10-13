#include "game/screens/OnlinePlay.h"

#include <algorithm>
#include <exception>

#include "engine/core/Log.h"

#include "game/screens/MatchInputs.h"

namespace gdl::game {
struct OnlinePlay::Host {
    Host() = default;
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    Host(Host&&) = delete;
    Host& operator=(Host&&) = delete;
    LevelWorld world;
    PlayScene scene;
    ProjectileResources projectiles;
    PickupResources pickups;
    FixtureResources fixtures;
    PlayReplication driver;
    ~Host() { scene.close(); }
};
OnlinePlay::OnlinePlay() = default;
OnlinePlay::~OnlinePlay() {
    close();
}

std::optional<u32> OnlinePlay::sceneId(const LevelCatalog& catalog, const LevelRef& level) {
    if (level.isTower()) {
        return level.name == "L1" && (level == LevelRef::tower() || catalog.byName("L1") == level)
                   ? std::optional<u32>{14 * 256 + 1}
                   : std::nullopt;
    }
    if (level.realmId < 0 || level.realmId >= 255 || level.index < 0 || level.index >= 255 ||
        catalog.byName(level.name) != level) {
        return std::nullopt;
    }
    return static_cast<u32>((level.realmId + 1) * 256 + level.index + 1);
}
std::optional<LevelRef> OnlinePlay::levelOf(const LevelCatalog& catalog, u32 scene) {
    if (scene == 14 * 256 + 1) {
        return LevelRef::tower();
    }
    if (scene == 0 || scene > 65535 || scene % 256 == 0) {
        return std::nullopt;
    }
    const auto realmId = static_cast<s32>(scene / 256) - 1;
    const auto index = static_cast<usize>(scene % 256 - 1);
    for (const auto& realm : catalog.realms()) {
        if (realm.id != realmId || realm.id == LevelRef::kTowerRealm ||
            index >= realm.levels.size()) {
            continue;
        }
        const auto level = catalog.byName(realm.levels[index]);
        if (level && sceneId(catalog, *level) == scene) {
            return level;
        }
    }
    return std::nullopt;
}
bool OnlinePlay::open(RenderDevice& device, const GameContext& context, OnlineSession& session,
                      OnlineParty& selection, const LevelRef& initial, const PlayOptions& options) {
    const auto& match = session.match();
    const bool initialLoad = !session.host() && match.phase() == MatchSession::Phase::Loading &&
                             match.context().transition == MatchTransition::Start;
    if (m_phase != Phase::Closed || session.phase() != OnlineSession::Phase::Active ||
        (match.phase() != MatchSession::Phase::Lobby && !initialLoad) ||
        context.levels == nullptr || !context.levels->loaded() ||
        (context.config != nullptr && context.config->timing.tickRate != 60) ||
        !selection.members(session)) {
        return false;
    }
    const auto scene = sceneId(*context.levels, initial);
    if (session.host() && (!scene || !session.match().prepare(*scene))) {
        return false;
    }
    m_device = &device;
    m_context = context;
    m_vibrate = context.vibrate;
    m_unlockClasses = context.unlockClasses;
    m_session = &session;
    m_selection = &selection;
    m_options = options;
    // Live remote characters have no disk authority. Apply only acknowledged
    // local checkpoints and newly earned awards through the owning application.
    m_context.unlockClasses = {};
    m_context.saveSettings = {};
    m_context.tower = nullptr;
    // The simulation emits room-seat cues even when the host has no controller.
    // Only the owning machine resolves capability, profile, and physical device.
    m_context.vibrate = [&session](s32 seat, s32 frames, ControlFeedback cue) {
        auto& active = session.match();
        if (active.phase() == MatchSession::Phase::Running &&
            active.context().transition != MatchTransition::Shop) {
            active.feedback().emit(seat, frames, cue);
        }
    };
    m_phase = Phase::Loading;
    return true;
}
void OnlinePlay::quiet() {
    m_prediction.clear();
    m_projectilePrediction.clear();
    m_potionPrediction.clear();
    m_effectPrediction.clear();
    m_presentEffects = false;
    m_latencyReportAt = {};
    m_audio.clear();
    if (m_host) {
        m_host->scene.pauseGameplaySounds();
    }
    if (m_context.stopVibration) {
        m_context.stopVibration();
    }
}
void OnlinePlay::close() {
    quiet();
    m_entry.close();
    m_completion.close();
    m_shop.close();
    m_afterShop = false;
    m_saveAfterShop = false;
    m_entryEpoch = 0;
    m_completionEpoch = 0;
    m_host.reset();
    m_parent.reset();
    m_level.reset();
    m_secretParent.reset();
    m_replica.clear();
    m_clock.clear();
    m_device = nullptr;
    m_context = {};
    m_vibrate = {};
    m_unlockClasses = {};
    m_session = nullptr;
    m_selection = nullptr;
    m_options = {};
    m_travelMembers.clear();
    m_phase = Phase::Closed;
    m_failure = Failure::None;
    m_outcome = PlayReplication::Result::Held;
    m_epoch = 0;
    m_pausedDevices = 0;
}
OnlinePlay::Phase OnlinePlay::fail(Failure reason) {
    quiet();
    m_entry.close();
    m_completion.close();
    m_failure = reason;
    m_phase = Phase::Failed;
    if (m_session != nullptr) {
        m_session->leave();
    }
    return m_phase;
}
bool OnlinePlay::load(const SessionInputs::Frame& devices) {
    auto& match = m_session->match();
    const auto& context = match.context();
    if (context.transition == MatchTransition::Shop) {
        return loadShop();
    }
    const auto level = levelOf(*m_context.levels, context.scene);
    if (!level) {
        fail(Failure::Scene);
        return false;
    }
    const bool enteringSecret = context.transition == MatchTransition::SecretEnter;
    const bool returningSecret = context.transition == MatchTransition::SecretReturn;
    const bool travelling =
        context.transition == MatchTransition::Travel || enteringSecret || returningSecret;
    const auto* profiles = travelling ? match.travelParty() : m_session->party();
    if (travelling && profiles == nullptr) {
        return false; // Reliable profiles can arrive in a later receive batch.
    }
    if (travelling) {
        const auto* admitted = m_session->party();
        if (admitted == nullptr || profiles == nullptr) {
            fail(Failure::Party);
            return false;
        }
        for (usize seat = 0; seat < profiles->size(); ++seat) {
            const auto& next = (*profiles)[seat];
            const auto& original = (*admitted)[seat];
            if (next.has_value() != original.has_value() ||
                (next && (next->name != original->name || next->character != original->character ||
                          next->color != original->color))) {
                fail(Failure::Party);
                return false;
            }
        }
        if (m_afterShop) {
            if (!m_selection->checkpoint(*m_session, *profiles)) {
                fail(Failure::Party);
                return false;
            }
            if (m_saveAfterShop && m_context.saveLocalParty) {
                m_context.saveLocalParty(m_selection->localMembers());
            }
            // Slot allocation is local-only, and must survive the next host scene.
            if (match.host()) {
                for (auto& member : m_travelMembers) {
                    if (const auto device =
                            m_selection->device(*m_session, static_cast<u8>(member.player))) {
                        for (const auto& local : m_selection->localMembers()) {
                            if (local.player == *device) {
                                member.slot = local.slot;
                            }
                        }
                    }
                }
            }
            m_afterShop = false;
            m_saveAfterShop = false;
            m_shop.close();
        }
    }
    try {
        if (context.transition != MatchTransition::Resume) {
            if ((m_epoch != 0) != travelling || profiles == nullptr) {
                fail(Failure::Scene);
                return false;
            }
            if (m_entryEpoch != context.epoch) {
                if (enteringSecret) {
                    if (!level->isSecret() || !m_level || m_level->isSecret() || m_secretParent ||
                        (match.host() && !m_host)) {
                        fail(Failure::Scene);
                        return false;
                    }
                    m_secretParent = m_level;
                    if (match.host()) {
                        m_host->scene.suspendForChallenge();
                        m_parent = std::move(m_host);
                    }
                } else if (returningSecret) {
                    if (!m_level || !m_level->isSecret() || !m_secretParent ||
                        *m_secretParent != *level || (match.host() && !m_parent)) {
                        fail(Failure::Scene);
                        return false;
                    }
                    if (!m_selection->checkpoint(*m_session, *profiles)) {
                        fail(Failure::Party);
                        return false;
                    }
                    // Award and save during the exit transition, before reopening the
                    // suspended world. Never import remote/preloaded unlock masks.
                    const s32 character = SecretChallenge::classFor(m_level->index);
                    if (context.challengeWon && character >= 8 && character < kClassCount) {
                        const auto reward = static_cast<u16>(1U << static_cast<u32>(character - 8));
                        for (auto& member : m_selection->localMembers()) {
                            member.save.classUnlock |= reward;
                        }
                        if (m_unlockClasses) {
                            m_unlockClasses(reward);
                        }
                        if (m_context.config != nullptr && m_context.config->game.autoSave &&
                            m_context.saveLocalParty) {
                            m_context.saveLocalParty(m_selection->localMembers());
                        }
                        if (match.host()) {
                            for (auto& member : m_travelMembers) {
                                const auto device =
                                    m_selection->device(*m_session, static_cast<u8>(member.player));
                                if (!device) {
                                    continue;
                                }
                                const s32 localDevice = *device;
                                for (const auto& local : m_selection->localMembers()) {
                                    if (local.player == localDevice) {
                                        member.slot = local.slot;
                                    }
                                }
                            }
                        }
                    }
                } else if (level->isSecret() || m_secretParent) {
                    fail(Failure::Scene);
                    return false;
                }
                // Dispose the old scene before entry audio starts, but preserve its
                // party checkpoint. The guest never constructs a gameplay scene.
                m_host.reset();
                m_completion.close();
                m_effectPrediction.clear();
                m_presentEffects = false;
                m_replica.clear();
                m_clock.clear();
                std::vector<PartyMember> preview;
                for (usize seat = 0; seat < profiles->size(); ++seat) {
                    if (const auto& profile = (*profiles)[seat]) {
                        PartyMember member;
                        member.player = static_cast<s32>(seat);
                        member.save = profile->gameplayCopy();
                        member.fallen = profile->progress.health <= 0;
                        preview.push_back(std::move(member));
                    }
                }
                m_entry.open(*m_device, m_context, *level, preview, returningSecret);
                m_entryEpoch = context.epoch;
            } else {
                const auto local = m_selection->inputs(*m_session, devices);
                if (!local) {
                    fail(Failure::Input);
                    return false;
                }
                if (m_entry.phase() == OnlineLevelEntry::Phase::Movie) {
                    const auto seats = m_session->seats();
                    for (usize index = 0; index < seats.size(); ++index) {
                        if ((*local)[index].movieSkipPressed &&
                            !match.requestMovieSkip(seats[index])) {
                            fail(Failure::Input);
                            return false;
                        }
                    }
                }
                m_entry.update(*m_device, m_context, match.movieSkipReady());
            }
            if (m_entry.phase() == OnlineLevelEntry::Phase::Failed) {
                fail(Failure::Assets);
                return false;
            }
            if (m_entry.phase() != OnlineLevelEntry::Phase::Ready) {
                return false;
            }
        }
        if (context.transition == MatchTransition::Resume) {
            if (m_epoch == 0 ||
                (match.host() ? !m_host || !m_host->driver.bind(m_host->scene, match,
                                                                m_host->pickups, m_host->fixtures)
                              : !m_replica.resume(context) || !m_clock.begin(match))) {
                fail(Failure::Scene);
                return false;
            }
        } else if ((m_epoch != 0) != travelling) {
            fail(Failure::Scene);
            return false;
        } else if (match.host() && returningSecret) {
            const SoundPlayer::Capture capture(
                m_context.sounds, [&match](const auto& event) { match.audio().emit(event); });
            m_host = std::move(m_parent);
            m_host->scene.resumeFromChallenge(m_travelMembers);
            if (!m_host->driver.bind(m_host->scene, match, m_host->pickups, m_host->fixtures)) {
                fail(Failure::Scene);
                return false;
            }
        } else if (match.host()) {
            const SoundPlayer::Capture capture(
                m_context.sounds, [&match](const auto& event) { match.audio().emit(event); });
            const auto members =
                travelling ? std::optional{m_travelMembers} : m_selection->members(*m_session);
            auto candidate = std::make_unique<Host>();
            if (!members || !candidate->world.load(*m_device, m_context.unpackedRoot, *level) ||
                !candidate->scene.open(*m_device, m_context, candidate->world, *members,
                                       m_options) ||
                !candidate->scene.bindReplicationResources(
                    candidate->projectiles, candidate->pickups, candidate->fixtures) ||
                !candidate->driver.bind(candidate->scene, match, candidate->pickups,
                                        candidate->fixtures)) {
                fail(Failure::Assets);
                return false;
            }
            m_host = std::move(candidate);
        } else {
            if (profiles == nullptr ||
                !m_replica.open(*m_device, m_context.unpackedRoot, *level, context, *profiles,
                                m_context.strings) ||
                !m_clock.begin(match)) {
                fail(Failure::Assets);
                return false;
            }
        }
        if (!match.host()) {
            const auto* world = m_replica.world();
            m_audio.open(m_context.unpackedRoot, m_context.sounds, world->audio(),
                         world->level() != nullptr ? world->level()->musicVolume : 1.0f);
            if (!m_audio.beginPrediction(context, match.localPeer())) {
                fail(Failure::Presentation);
                return false;
            }
            for (usize seat = 0; seat < context.owners.size(); ++seat) {
                if (context.owners[seat] == match.localPeer()) {
                    // Decode local release cues while loading, not on the first attack.
                    // Missing optional audio falls back to the host's ordinary stream.
                    m_audio.prepareWeapon(static_cast<u8>(seat),
                                          m_replica.character(static_cast<u8>(seat)));
                }
            }
        }
    } catch (const std::exception& error) {
        log::error("Online stage load failed: {}", error.what());
        fail(Failure::Assets);
        return false;
    }
    m_epoch = context.epoch;
    m_travelMembers.clear();
    m_outcome = PlayReplication::Result::Held;
    if (!match.loaded()) {
        fail(Failure::Session);
        return false;
    }
    if (returningSecret) {
        m_secretParent.reset();
    }
    m_level = *level;
    log::info("Online stage loaded: {} (epoch {}, {})", level->name, m_epoch,
              match.host() ? "host" : "guest");
    return true;
}
bool OnlinePlay::travel() {
    if (!m_host || m_outcome != PlayReplication::Result::Travel) {
        return false;
    }
    const auto& scene = m_host->scene;
    const bool returningSecret = scene.secretTravel() && m_host->world.ref().isSecret();
    const bool enteringSecret = scene.secretTravel() && !returningSecret;
    if (returningSecret && (!m_parent || !m_secretParent)) {
        fail(Failure::Scene);
        return false;
    }
    const auto destination = returningSecret ? *m_secretParent : scene.destination();
    const auto* info = m_host->world.level();
    const auto id = sceneId(*m_context.levels, destination);
    auto members = scene.party();
    const auto& world = m_host->world;
    if (!world.isTower() && !scene.secretTravel()) {
        recordLevelBeaten(members, world.ref().realmId, world.ref().index,
                          info != nullptr ? info->rune : 0, info != nullptr ? info->legend : 0);
    }
    MatchParty profiles;
    for (const auto& member : members) {
        if (member.player < 0 || member.player >= static_cast<s32>(profiles.size()) ||
            profiles[static_cast<usize>(member.player)]) {
            fail(Failure::Party);
            return false;
        }
        auto& profile = profiles[static_cast<usize>(member.player)];
        profile = CharacterProfile::capture(member.save);
        if (!profile) {
            fail(Failure::Party);
            return false;
        }
    }
    if (enteringSecret || returningSecret) {
        const bool won = returningSecret && scene.challenge().won();
        if (!id || !m_session->match().prepare(*id,
                                               enteringSecret ? MatchTransition::SecretEnter
                                                              : MatchTransition::SecretReturn,
                                               &profiles, nullptr, won)) {
            fail(Failure::Session);
            return false;
        }
    } else if (!world.isTower() && destination.isTower()) {
        ShopSetup setup;
        setup.source = m_session->match().context().scene;
        setup.destination = id.value_or(0);
        setup.expanded = m_context.config != nullptr && m_context.config->game.expandedShop;
        setup.ending = info != nullptr && (info->bossType == 43 || info->bossType == 44) &&
                       scene.victory().finished();
        setup.maxima = info != nullptr ? info->shopMaxima : std::array<s32, 3>{1000, 100, 1000};
        for (const auto& result : scene.levelResults()) {
            setup.totals[static_cast<usize>(result.player)] = result.totals;
        }
        for (const auto& member : members) {
            if (member.fallen) {
                setup.fallen |= static_cast<u8>(1U << static_cast<u32>(member.player));
            }
        }
        if (!id ||
            !m_session->match().prepare(setup.source, MatchTransition::Shop, &profiles, &setup)) {
            fail(Failure::Session);
            return false;
        }
    } else if (!id || !m_session->match().prepare(*id, MatchTransition::Travel, &profiles)) {
        fail(Failure::Session);
        return false;
    }
    m_travelMembers = std::move(members);
    m_options = {};
    m_options.welcome = false;
    m_options.arriving = true;
    m_options.arrivalWorld = static_cast<u32>(std::max(world.ref().realmId, 0));
    quiet();
    m_phase = Phase::Loading;
    return true;
}
bool OnlinePlay::loadShop() {
    auto& match = m_session->match();
    const auto* profiles = match.travelParty();
    const auto* setup = match.shop().setup();
    if (profiles == nullptr || setup == nullptr) {
        return false;
    }
    const auto source = levelOf(*m_context.levels, setup->source);
    const auto destination = levelOf(*m_context.levels, setup->destination);
    if (!source || !destination || setup->source != match.context().scene || source->isTower() ||
        source->isSecret() || !destination->isTower() || m_secretParent) {
        fail(Failure::Scene);
        return false;
    }
    if (m_completionEpoch != match.context().epoch) {
        WorldData data;
        if (!data.load(m_context.unpackedRoot / source->worldDataFile())) {
            fail(Failure::Assets);
            return false;
        }
        const auto* info = data.level(source->name);
        if (info == nullptr || (setup->ending && info->bossType != 43 && info->bossType != 44)) {
            fail(Failure::Scene);
            return false;
        }
        quiet();
        m_host.reset();
        m_replica.clear();
        m_clock.clear();
        m_entry.close();
        m_completion.open(*m_device, m_context, *source, info->bossType, setup->ending);
        m_completionEpoch = match.context().epoch;
    } else {
        m_completion.update(m_context);
    }
    if (m_completion.phase() == OnlineCompletion::Phase::Failed) {
        fail(Failure::Assets);
        return false;
    }
    if (m_completion.phase() != OnlineCompletion::Phase::Ready) {
        return false;
    }
    std::vector<PartyMember> members;
    std::vector<LevelResults> results;
    const auto* admitted = m_session->party();
    if (admitted == nullptr) {
        fail(Failure::Party);
        return false;
    }
    for (usize seat = 0; seat < profiles->size(); ++seat) {
        const auto& profile = (*profiles)[seat];
        const auto& original = (*admitted)[seat];
        if (profile.has_value() != original.has_value() ||
            (profile &&
             (profile->name != original->name || profile->character != original->character ||
              profile->color != original->color))) {
            fail(Failure::Party);
            return false;
        }
        if (profile) {
            PartyMember member;
            member.player = static_cast<s32>(seat);
            member.save = profile->gameplayCopy();
            member.fallen = (setup->fallen & (1U << seat)) != 0;
            members.push_back(std::move(member));
            results.push_back({static_cast<s32>(seat), setup->totals[seat]});
        }
    }
    if (match.host()) {
        members = m_travelMembers;
    }
    m_shopConfig = m_context.config != nullptr ? *m_context.config : GameConfig{};
    m_shopConfig.game.expandedShop = setup->expanded;
    auto context = m_context;
    context.config = &m_shopConfig;
    const auto visit = m_completion.selection().visit;
    if (!m_shop.open(*m_device, context, members, results, setup->maxima, source->name, visit)) {
        fail(Failure::Assets);
        return false;
    }
    m_epoch = match.context().epoch;
    m_afterShop = true;
    m_saveAfterShop = visit != ShopVisit::Completion;
    m_travelMembers.clear();
    m_pausedDevices = 0;
    if (!match.loaded()) {
        fail(Failure::Session);
        return false;
    }
    return true;
}
OnlinePlay::Phase OnlinePlay::updateShop(const SessionInputs::Frame& devices) {
    m_completion.close();
    auto& match = m_session->match();
    auto& sync = match.shop();
    const auto local = m_selection->inputs(*m_session, devices);
    if (!local) {
        return fail(Failure::Input);
    }
    for (usize index = 0; index < m_session->seats().size(); ++index) {
        const auto seat = m_session->seats()[index];
        auto input = (*local)[index].menu;
        const auto lane =
            std::ranges::find(m_shop.session().lanes(), static_cast<s32>(seat),
                              [](const ShopLane& value) { return value.member.player; });
        if (lane != m_shop.session().lanes().end() && lane->phase == ShopPhase::Shopping &&
            m_context.config != nullptr) {
            const auto device = m_selection->device(*m_session, seat);
            if (!device) {
                return fail(Failure::Input);
            }
            input = AfterLevelScene::captureShopInput(input, *m_context.config, *device);
        }
        const auto focus = m_shop.captureOnlinePointer(static_cast<s32>(seat), input);
        if (lane == m_shop.session().lanes().end() ||
            !sync.sample(seat, input, focus ? static_cast<s32>(*focus) : -1,
                         static_cast<u8>(lane->phase), lane->cursor)) {
            return fail(Failure::Input);
        }
    }
    m_phase = Phase::Shopping;
    auto step = match.host() ? sync.advance() : sync.take();
    if (step) {
        for (usize seat = 0; seat < step->focus.size(); ++seat) {
            if (match.host() && step->observedPhase[seat] != 255) {
                const auto lane =
                    std::ranges::find(m_shop.session().lanes(), static_cast<s32>(seat),
                                      [](const ShopLane& value) { return value.member.player; });
                if (lane == m_shop.session().lanes().end() ||
                    step->observedPhase[seat] != static_cast<u8>(lane->phase)) {
                    step->inputs[seat] = {};
                    step->focus[seat] = -1;
                } else if (step->focus[seat] == -1 && step->observedCursor[seat] != lane->cursor) {
                    // A delayed Buy/Sell must not transact against a different row.
                    step->inputs[seat].select = false;
                    step->inputs[seat].back = false;
                }
            }
            if (step->focus[seat] >= 0 &&
                !m_shop.focusOnline(static_cast<s32>(seat),
                                    static_cast<usize>(step->focus[seat]))) {
                if (!match.host()) {
                    return fail(Failure::Presentation);
                }
                step->focus[seat] = -1;
                step->inputs[seat].select = false;
                step->inputs[seat].back = false;
            }
        }
        const bool finished = m_shop.update(1.0 / 60, step->inputs, &step->potions,
                                            match.host() ? &step->inputs : nullptr, !match.host());
        if (match.host() && !sync.publish(*step)) {
            return fail(Failure::Session);
        }
        if (finished && !sync.finish()) {
            return fail(Failure::Session);
        }
    }
    if (match.host() && sync.finished()) {
        MatchParty profiles;
        auto members = m_shop.session().party();
        for (auto& member : members) {
            profiles[static_cast<usize>(member.player)] = CharacterProfile::capture(member.save);
            member.resultsCheckpoint.reset();
        }
        const auto destination = sync.setup()->destination;
        if (!match.prepare(destination, MatchTransition::Travel, &profiles)) {
            return fail(Failure::Session);
        }
        m_travelMembers = std::move(members);
        m_phase = Phase::Loading;
    }
    return m_phase;
}
OnlinePlay::Phase OnlinePlay::update(const SessionInputs::Frame& devices) {
    const auto result = updateFrame(devices);
    // Poll before simulation, then submit its output in the same update. Waiting
    // until the next poll adds a full 60 Hz tick on each side of the connection.
    // Keep this outside the frame's branches so shop/travel/loading also flush.
    if (m_session != nullptr && result != Phase::Failed && !m_session->flush()) {
        return fail(Failure::Session);
    }
    return result;
}
OnlinePlay::Phase OnlinePlay::updateFrame(const SessionInputs::Frame& devices) {
    if (m_session == nullptr) {
        return m_phase;
    }
    auto gameplay = devices;
    for (usize device = 0; device < gameplay.size(); ++device) {
        if ((m_pausedDevices & (1U << device)) != 0) {
            gameplay[device] = {};
        }
    }
    m_session->update(1.0 / 60);
    if (m_phase == Phase::Failed) {
        return m_phase;
    }
    auto& match = m_session->match();
    if (m_session->phase() != OnlineSession::Phase::Active ||
        match.phase() == MatchSession::Phase::Stopped) {
        return fail(Failure::Session);
    }
    if (match.phase() == MatchSession::Phase::Loading && match.context().epoch != m_epoch) {
        quiet();
        m_phase = Phase::Loading;
        if (!load(gameplay)) {
            return m_phase;
        }
    }
    if (match.phase() == MatchSession::Phase::Paused) {
        if (m_phase != Phase::Paused) {
            quiet();
        }
        m_phase = Phase::Paused;
        return m_phase;
    }
    if (match.phase() != MatchSession::Phase::Running || m_phase == Phase::Finished) {
        return m_phase;
    }
    if (m_epoch == 0 || m_epoch != match.context().epoch) {
        return fail(Failure::Scene);
    }
    if (match.context().transition == MatchTransition::Shop) {
        return updateShop(gameplay);
    }
    // New epochs reset gates. Reapply local pause state before consuming any cue.
    for (const auto seat : m_session->seats()) {
        if (const auto local = m_selection->device(*m_session, seat)) {
            match.feedback().setEnabled(seat,
                                        (m_pausedDevices & (1U << static_cast<u32>(*local))) == 0);
        }
    }
    feedback();
    const auto local = m_selection->inputs(*m_session, gameplay);
    if (!local) {
        return fail(Failure::Input);
    }
    m_phase = Phase::Playing;
    if (!match.host()) {
        if (!m_audio.consume(match.audio())) {
            return fail(Failure::Presentation);
        }
        if (const auto* state = shown()) {
            m_audio.music(*state);
        }
        const auto commands = MatchInputs::commands(match, *local);
        match.latency().update();
        match.latency().motionRequest(commands, shown());
        if (!match.sample(commands)) {
            return fail(Failure::Input);
        }
        if (m_prediction.epoch() != match.context().epoch) {
            const auto* world = m_replica.world();
            const auto* view = m_replica.view();
            if (world == nullptr || view == nullptr) {
                return fail(Failure::Presentation);
            }
            m_prediction.begin(match.context(), match.localPeer(), world->collision(),
                               &world->scene(), &world->layout());
            m_projectilePrediction.begin(match.context(), match.localPeer(), world->collision(),
                                         &world->scene(), &world->layout());
            if (const auto* weapons = m_replica.weapons()) {
                if (const auto* resources = m_replica.projectiles()) {
                    m_potionPrediction.begin(match.context(), match.localPeer(), world->collision(),
                                             *weapons, *resources, &world->scene(),
                                             &world->layout());
                }
            }
            m_effectPrediction.begin(match.context(), match.localPeer());
            for (usize seat = 0; seat < match.context().owners.size(); ++seat) {
                if (match.context().owners[seat] == match.localPeer()) {
                    const auto* figure = view->actors().playerFigure(static_cast<u8>(seat));
                    if (figure != nullptr && figure->actionTree() != nullptr) {
                        const auto* stats = m_replica.classStats(static_cast<u8>(seat));
                        m_prediction.setPlayer(
                            static_cast<u8>(seat), *figure->actionTree(), stats,
                            stats != nullptr ? m_replica.character(static_cast<u8>(seat)) : -1);
                        const auto* effects = m_replica.turboEffects(static_cast<u8>(seat));
                        const auto* weapons = m_replica.weapons();
                        if (effects != nullptr && weapons != nullptr) {
                            m_effectPrediction.bind(
                                static_cast<u8>(seat), *effects, *figure, *weapons,
                                view->actors().playerScale(static_cast<u8>(seat)));
                        }
                        if (const auto* fire =
                                view->actors().fireShieldTree(static_cast<u8>(seat))) {
                            m_prediction.setFireShield(
                                static_cast<u8>(seat), *fire,
                                view->actors().playerScale(static_cast<u8>(seat)));
                        }
                    }
                }
            }
        }
        m_prediction.update(match.playback().latest(), match.sampledInputs());
        m_potionPrediction.update(match.playback().latest(), m_prediction, match.sampledInputs());
        m_effectPrediction.update(match.playback().latest(), m_prediction);
        if (const auto* authority = match.playback().latest()) {
            for (usize seat = 0; seat < authority->players.size(); ++seat) {
                const auto* replay = m_prediction.replayed(static_cast<u8>(seat));
                if (replay == nullptr || !authority->players[seat] ||
                    !authority->players[seat]->weapon) {
                    continue;
                }
                for (const auto& release : replay->releases) {
                    match.latency().localRelease({match.context().epoch, release.tick,
                                                  release.generation, release.motion.grant,
                                                  static_cast<u8>(seat), release.strong, 0,
                                                  release.emission});
                }
            }
        }
        if (const auto* resources = m_replica.projectiles()) {
            m_projectilePrediction.update(match.playback().latest(), m_prediction, *resources,
                                          match.sampledInputs());
            if (const auto* authority = match.playback().latest()) {
                std::vector<ShotIdentity> pending;
                if (m_predictMovement) {
                    for (const auto& release : m_projectilePrediction.soundReleases()) {
                        pending.push_back(release.origin);
                    }
                }
                if (!m_audio.reconcile(*authority, pending)) {
                    return fail(Failure::Presentation);
                }
                if (m_predictMovement) {
                    for (const auto& release : m_projectilePrediction.soundReleases()) {
                        m_audio.predict(release.origin, release.shots, release.weaponFlags);
                    }
                }
            }
        }
        const auto now = LatencyTelemetry::Clock::now();
        if (m_latencyReportAt == LatencyTelemetry::Time{}) {
            m_latencyReportAt = now;
        }
        if (now - m_latencyReportAt >= std::chrono::seconds(10)) {
            m_latencyReportAt = now;
            const auto& metrics = m_prediction.metrics();
            log::info("Netplay movement: lead={} ticks, pending={}, resolved deadline={} ms, "
                      "corrections={}, largest={} units, cuts={}, stalled={}",
                      match.inputLead(), metrics.pending, metrics.resolvedDeadlineMs,
                      metrics.corrections, metrics.largestCorrection, metrics.cuts,
                      metrics.stalled);
            log::info("Netplay latency: {}", match.latency().report());
        }
        return phase();
    }
    if (m_entry.phase() != OnlineLevelEntry::Phase::Closed) {
        m_entry.close();
    }
    std::vector<SoundPlayer::Event> sounds;
    std::vector<PlayerArsenal::WeaponSound> releases;
    bool overflow = false;
    {
        // The native cue precedes the completed checkpoint. Buffer only this step
        // so its identity uses the same post-step tick as projectile replication.
        const SoundPlayer::Capture capture(m_context.sounds, [&](const auto& event) {
            if (sounds.size() < AudioSync::kBudget) {
                sounds.push_back(event);
            } else {
                overflow = true;
            }
        });
        const auto weapons = m_host->scene.captureWeaponSounds([&](const auto& release) {
            if (releases.size() < AudioSync::kBudget) {
                releases.push_back(release);
            } else {
                overflow = true;
            }
        });
        m_outcome = m_host->driver.advance(match, *local, m_host->projectiles);
    }
    feedback();
    if (overflow || m_outcome == PlayReplication::Result::Failed) {
        return fail(Failure::Capture);
    }
    for (const auto& event : sounds) {
        std::optional<ShotIdentity> origin;
        const auto* state = m_host->driver.latest();
        if (state != nullptr && event.kind == SoundPlayer::Event::Kind::Play) {
            const auto release =
                std::ranges::find(releases, event.handle, &PlayerArsenal::WeaponSound::handle);
            if (release != releases.end() && release->owner >= 0 &&
                release->owner < static_cast<s32>(InputCommand::kSeats)) {
                const auto seat = static_cast<u8>(release->owner);
                origin = ShotIdentity{match.context().epoch,
                                      state->motion.tick,
                                      release->release.generation,
                                      match.context().grants[seat],
                                      seat,
                                      release->release.strong,
                                      0,
                                      release->release.emission};
            }
        }
        if (!match.audio().emit(event, origin)) {
            return fail(Failure::Capture);
        }
    }
    if (m_outcome != PlayReplication::Result::Advanced &&
        m_outcome != PlayReplication::Result::Held) {
        if (travel() || m_phase == Phase::Failed) {
            return m_phase;
        }
        quiet();
        m_phase = Phase::Finished;
    }
    return phase();
}
void OnlinePlay::feedback() {
    while (const auto event = m_session->match().feedback().take()) {
        const auto local = m_selection->device(*m_session, event->seat);
        if (local && m_vibrate && (m_pausedDevices & (1U << static_cast<u32>(*local))) == 0) {
            m_vibrate(*local, event->frames, event->feedback);
        }
    }
}
bool OnlinePlay::pause(s32 device) {
    if (m_session == nullptr || m_phase != Phase::Playing || device < -1 ||
        device >= static_cast<s32>(InputCommand::kSeats)) {
        return false;
    }
    u32 mask = 0;
    for (const auto seat : m_session->seats()) {
        if (const auto local = m_selection->device(*m_session, seat);
            local && (device == -1 || *local == device)) {
            mask |= 1U << static_cast<u32>(*local);
            m_session->match().feedback().setEnabled(seat, false);
        }
    }
    m_pausedDevices |= mask;
    if (mask != 0 && m_context.stopVibration) {
        m_context.stopVibration();
    }
    return mask != 0;
}
bool OnlinePlay::resume(s32 device) {
    if (device < -1 || device >= static_cast<s32>(InputCommand::kSeats)) {
        return false;
    }
    const u32 mask = device == -1 ? m_pausedDevices : (1U << static_cast<u32>(device));
    const bool captured = (m_pausedDevices & mask) != 0;
    m_pausedDevices &= ~mask;
    if (captured && m_session != nullptr) {
        for (const auto seat : m_session->seats()) {
            if (const auto local = m_selection->device(*m_session, seat);
                local && (mask & (1U << static_cast<u32>(*local))) != 0) {
                m_session->match().feedback().setEnabled(seat, true);
            }
        }
    }
    return captured;
}
void OnlinePlay::render(RenderDevice& device, const Mat4& projection, f32 width, f32 height,
                        f64 seconds, f32 frameBlend) {
    if (m_phase == Phase::Closed || m_phase == Phase::Failed) {
        return;
    }
    if (m_phase == Phase::Loading && m_completion.visible()) {
        m_completion.render(device, projection, width, height);
        return;
    }
    if (m_shop.isOpen()) {
        m_shop.render(device, projection, width, height);
        return;
    }
    if (m_phase == Phase::Loading && m_entry.visible()) {
        m_entry.render(device, projection, width, height);
        return;
    }
    if (m_host) {
        m_host->scene.render(device, projection, width, height, m_phase == Phase::Paused,
                             m_phase == Phase::Playing ? frameBlend : 1);
        return;
    }
    if (m_session == nullptr) {
        return;
    }
    EnemyPresentation presentation;
    if (auto sample = m_clock.sample(m_session->match(), seconds, &presentation)) {
        m_presentEffects = false;
        if (m_predictMovement) {
            m_prediction.present(*sample, frameBlend);
            m_projectilePrediction.present(*sample, frameBlend);
            m_potionPrediction.present(*sample, frameBlend);
            m_presentEffects = m_effectPrediction.present(*sample);
        }
        if (!m_replica.show(*sample, &presentation)) {
            if (const auto* view = m_replica.view()) {
                log::error("Online presentation rejected: {}", view->rejection(*sample));
                for (usize seat = 0; seat < sample->players.size(); ++seat) {
                    if (sample->players[seat]) {
                        const auto& animation = sample->players[seat]->animation;
                        const auto* figure = view->actors().playerFigure(static_cast<u8>(seat));
                        log::error(
                            "Pose seat {} action {} sequence {} frame {} generation {} frames {}",
                            seat, animation.action, animation.sequence, animation.frame,
                            animation.generation,
                            figure != nullptr &&
                                    animation.sequence < figure->actionTree()->sequences.size()
                                ? figure->actionTree()->sequences[animation.sequence].frames
                                : -1);
                    }
                }
            }
            fail(Failure::Presentation);
            return;
        }
        if (m_entry.phase() != OnlineLevelEntry::Phase::Closed) {
            m_entry.close();
        }
    }
    if (m_entry.visible()) {
        m_entry.render(device, projection, width, height);
        return; // Keep the preview until the first complete new-epoch checkpoint.
    }
    // Native texture sequences use game frames, not monitor frames.
    const f32 textureFrame =
        static_cast<f32>(m_clock.tick()) / 2 + static_cast<f32>(m_clock.fraction()) / 2;
    static const GameConfig kDefaultVideo;
    m_replica.draw(device, projection, width, height, textureFrame,
                   m_context.config != nullptr ? *m_context.config : kDefaultVideo,
                   m_predictMovement && m_presentEffects ? &m_effectPrediction : nullptr,
                   frameBlend);
    if (const auto* state = shown()) {
        m_session->match().latency().presented(*state);
    }
}
const PlayScene* OnlinePlay::hostScene() const {
    return m_host ? &m_host->scene : nullptr;
}
const CombatSnapshot* OnlinePlay::shown() const {
    if (m_host) {
        return m_host->driver.latest();
    }
    const auto* view = m_replica.view();
    return view != nullptr ? view->shown() : nullptr;
}
} // namespace gdl::game
