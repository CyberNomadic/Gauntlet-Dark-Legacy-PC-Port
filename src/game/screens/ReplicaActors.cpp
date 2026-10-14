#include "game/screens/ReplicaActors.h"

#include <algorithm>
#include <cmath>

namespace gdl::game {
namespace {
Mat4 placement(const Vec3& position, f32 yaw, f32 scale = 1) {
    return glm::scale(glm::rotate(glm::translate(Mat4{1}, position), yaw, Vec3{0, 1, 0}),
                      Vec3{scale});
}
} // namespace

bool ReplicaActors::begin(u64 epoch) {
    if (epoch == 0 || epoch <= m_epoch) {
        return false;
    }
    m_epoch = epoch;
    m_tick.reset();
    m_enemies.clear();
    for (auto& player : m_players) {
        player.shown = false;
        player.continuity = 0;
        player.portalPhase.reset();
        player.nativePose.reset();
        player.pose.bind(player.figure ? player.figure->actionTree() : nullptr);
    }
    return true;
}
void ReplicaActors::clear() {
    m_epoch = 0;
    m_tick.reset();
    m_players = {};
    m_portalSkin.clear();
    m_deathSkins.clear();
    m_enemies.clear();
}
void ReplicaActors::loadDeathSkins(RenderDevice& device, Enemies& enemies, ItemArchive& weapons) {
    m_deathSkins.clear();
    for (u32 skin = 1; skin <= Enemies::kDeathSkins.size(); ++skin) {
        for (const s32 kind : {-1, 5, 11}) {
            if ((skin == 6) != (kind != -1)) {
                continue;
            }
            auto* archive = kind == -1 ? &weapons : enemies.archive(kind);
            if (archive == nullptr) {
                continue;
            }
            for (const auto& animation : archive->trees.textureAnimations()) {
                if (animation.name != Enemies::kDeathSkins[skin - 1]) {
                    continue;
                }
                const auto first = animation.source >= 0
                                       ? std::optional{static_cast<u32>(animation.source)}
                                       : archive->textures.find(animation.frameName);
                if (first && *first < archive->textures.size()) {
                    auto& frames = m_deathSkins[{skin, kind}];
                    for (u32 frame = 0; frame < static_cast<u32>(std::max(0, animation.frames)) &&
                                        *first + frame < archive->textures.size();
                         ++frame) {
                        frames.push_back(&archive->textures.texture(device, *first + frame));
                    }
                }
                break;
            }
        }
    }
}
bool ReplicaActors::setPlayer(u8 seat, u32 grant, std::unique_ptr<PlayerFigure> figure, f32 scale) {
    if (seat >= m_players.size() || grant == 0 || !figure || !std::isfinite(scale) || scale <= 0 ||
        scale > 16) {
        return false;
    }
    auto& player = m_players[seat];
    player = {};
    player.grant = grant;
    player.scale = scale;
    player.pose.bind(figure->actionTree());
    player.resources.bindEarned(*figure);
    player.figure = std::move(figure);
    return true;
}
bool ReplicaActors::bindCompanions(RenderDevice& device, ItemArchive& powerups,
                                   ItemArchive* weapons) {
    if (m_tick) {
        return false;
    }
    std::array<CompanionResources, InputCommand::kSeats> ready;
    for (usize seat = 0; seat < m_players.size(); ++seat) {
        const auto& player = m_players[seat];
        if (player.figure) {
            ready[seat].bindEarned(*player.figure);
            if (weapons == nullptr || !ready[seat].bindPowerups(device, powerups, weapons) ||
                !player.figure->bindArmEquipment(device, powerups, *weapons)) {
                return false;
            }
        }
    }
    for (usize seat = 0; seat < m_players.size(); ++seat) {
        m_players[seat].resources = std::move(ready[seat]);
    }
    return true;
}
bool ReplicaActors::companionsReady(const CombatSnapshot& snapshot) const {
    for (usize seat = 0; seat < m_players.size(); ++seat) {
        const auto& player = snapshot.players[seat];
        if (!player) {
            continue;
        }
        if (player->armEquipment != 0 &&
            (!m_players[seat].figure ||
             !m_players[seat].figure->armEquipmentReady(player->armEquipment))) {
            return false;
        }
        const auto& companions = player->companions;
        for (usize slot = 0; slot < companions.size(); ++slot) {
            const auto& companion = companions[slot];
            if (companion && !m_players[seat].resources.accepts(slot, *companion)) {
                return false;
            }
        }
    }
    return true;
}
void ReplicaActors::loadPortalSkin(RenderDevice& device, TextureSet& weapons) {
    m_portalSkin.loadSkin(device, weapons);
}
std::optional<ReplicaActors::NativePoses>
ReplicaActors::preparePresentation(const CombatSnapshot& snapshot, Enemies& resources,
                                   const EnemyPresentation& presentation) const {
    if (!presentation.matches(snapshot)) {
        return std::nullopt;
    }
    NativePoses result;
    for (usize seat = 0; seat < presentation.players.size(); ++seat) {
        const auto& native = presentation.players[seat];
        if (!native) {
            continue;
        }
        const auto& player = m_players[seat];
        const auto* tree = player.figure ? player.figure->actionTree() : nullptr;
        TreePose pose;
        if (player.grant != native->grant || native->life == ReplicaPlayerLife::InTower ||
            !ReplicaPose::accepts(tree, native->animation) || !pose.restore(*tree, native->pose)) {
            return std::nullopt;
        }
        result.players[seat] = std::move(pose);
    }
    result.enemies.reserve(presentation.enemies.size());
    for (const auto& interval : presentation.enemies) {
        if (!interval.nativePose) {
            continue;
        }
        const auto& enemy = interval.to;
        const Enemies::Appearance appearance{
            static_cast<s32>(enemy.kind), static_cast<s32>(enemy.tier),
            static_cast<s32>(enemy.variant), enemy.life == ReplicaEnemyLife::Asleep};
        const auto* tree = resources.appearanceTree(appearance);
        TreePose pose;
        if (!ReplicaPose::accepts(tree, enemy.animation) ||
            !pose.restore(*tree, *interval.nativePose)) {
            return std::nullopt;
        }
        result.enemies.emplace_back(enemy.instance, std::move(pose));
    }
    return result;
}
bool ReplicaActors::acceptsPresentation(const CombatSnapshot& snapshot, Enemies& resources,
                                        const EnemyPresentation& presentation) const {
    return snapshot.valid() && preparePresentation(snapshot, resources, presentation).has_value();
}
bool ReplicaActors::show(const CombatSnapshot& snapshot, Enemies& resources,
                         const EnemyPresentation* presentation,
                         std::span<const WorldCollision> presentationFloors) {
    if (m_epoch == 0 || snapshot.motion.epoch != m_epoch || !snapshot.valid() ||
        (m_tick && snapshot.motion.tick < *m_tick) || !companionsReady(snapshot) ||
        (presentation != nullptr && !presentation->matches(snapshot))) {
        return false;
    }
    auto native = presentation != nullptr ? preparePresentation(snapshot, resources, *presentation)
                                          : std::optional<NativePoses>{NativePoses{}};
    if (!native) {
        return false;
    }
    m_tick = snapshot.motion.tick;
    for (usize seat = 0; seat < m_players.size(); ++seat) {
        auto& player = m_players[seat];
        const auto& state = snapshot.players[seat];
        const auto& motion = snapshot.motion.players[seat];
        player.shown = false;
        player.nativePose.reset();
        player.companions = {};
        if (!state || !motion || !player.figure || player.grant != motion->grant ||
            state->life == ReplicaPlayerLife::InTower ||
            (state->portalPhase && *state->portalPhase >= 1) ||
            state->animation.action >= PlayerAnimator::kActionCount) {
            player.pose.bind(player.figure ? player.figure->actionTree() : nullptr);
            player.continuity = 0;
            continue;
        }
        if (player.continuity != motion->continuity) {
            player.pose.bind(player.figure->actionTree());
            player.continuity = motion->continuity;
        }
        player.shown = state->transitionPose && state->animation.transition < 1
                           ? player.pose.show(state->animation, *state->transitionPose)
                           : player.pose.show(state->animation);
        player.nativePose = std::move(native->players[seat]);
        if (player.nativePose) {
            player.shown = true;
        }
        player.action = static_cast<PlayerAnimator::Action>(state->animation.action);
        player.hitFlash = state->hitFlash;
        player.weaponHidden = state->weaponHidden;
        player.armEquipment = state->armEquipment;
        player.portalPhase = state->portalPhase;
        player.shadow = state->shadow;
        player.placement = placement(motion->position, motion->yaw, player.scale);
        if (player.portalPhase) {
            player.placement = PortalDeparture::transformAt(player.placement, *player.portalPhase);
        }
        player.companions = state->companions;
    }
    std::erase_if(m_enemies, [&](const auto& row) {
        const auto found =
            std::ranges::lower_bound(snapshot.enemies, row.first, {}, &EnemyCombatState::instance);
        return found == snapshot.enemies.end() || found->instance != row.first;
    });
    for (const auto& state : snapshot.enemies) {
        auto& enemy = m_enemies[state.instance];
        enemy.appearance = {static_cast<s32>(state.kind), static_cast<s32>(state.tier),
                            static_cast<s32>(state.variant),
                            state.life == ReplicaEnemyLife::Asleep};
        const auto* tree = resources.appearanceTree(enemy.appearance);
        if (tree != enemy.tree) {
            enemy.tree = tree;
            enemy.pose.bind(tree);
        }
        const bool statue = state.kind == kDeathKind && enemy.appearance.asleep;
        enemy.shown = statue ? enemy.pose.rest() : enemy.pose.show(state.animation);
        enemy.animation = state.animation;
        enemy.nativePose.reset();
        enemy.nativeTextureFrame.reset();
        enemy.hitFlash = state.hitFlash;
        enemy.alpha = state.alpha;
        enemy.dying = state.life == ReplicaEnemyLife::Dying;
        enemy.deathSkin = nullptr;
        const auto skin = m_deathSkins.find(
            {state.deathSkin, state.deathSkin == 6 ? static_cast<s32>(state.kind) : -1});
        if (skin != m_deathSkins.end() && state.deathFrame < skin->second.size()) {
            enemy.deathSkin = skin->second[state.deathFrame];
        }
        Vec3 shown = state.position;
        if (presentation != nullptr && presentationFloors.size() == 3) {
            const auto interval =
                std::ranges::lower_bound(presentation->enemies, state.instance, {},
                                         [](const auto& row) { return row.from.instance; });
            if (interval != presentation->enemies.end() &&
                interval->from.instance == state.instance &&
                state.animation.action < kEnemyActionCount) {
                if (interval->nativeBlend) {
                    // Native swarm floor correction uses the current simulation
                    // collision, not a fractional or preceding 60 Hz floor.
                    shown = Enemies::supportedPresentation(
                        &presentationFloors[1], interval->from.position, interval->to.position,
                        *interval->nativeBlend, static_cast<EnemyAction>(state.animation.action),
                        state.life == ReplicaEnemyLife::Active);
                } else {
                    shown = Enemies::supportedPresentation(
                        &presentationFloors[2], interval->from.position, interval->to.position,
                        presentation->blend, static_cast<EnemyAction>(state.animation.action),
                        state.life == ReplicaEnemyLife::Active, presentationFloors.data(),
                        &presentationFloors[1]);
                }
            }
        }
        if (presentation != nullptr) {
            const auto interval =
                std::ranges::lower_bound(presentation->enemies, state.instance, {},
                                         [](const auto& row) { return row.from.instance; });
            if (interval != presentation->enemies.end() &&
                interval->from.instance == state.instance && interval->nativePose) {
                const auto prepared = std::ranges::lower_bound(
                    native->enemies, state.instance, {}, [](const auto& row) { return row.first; });
                enemy.nativePose = std::move(prepared->second);
                enemy.nativeTextureFrame = interval->nativeTextureFrame;
            }
        }
        enemy.placement = placement(shown, state.yaw, state.scale);
    }
    return true;
}
void ReplicaActors::draw(RenderDevice& device, Enemies& resources, const Mat4& clip,
                         const WorldLighting& lighting, const CameraFrame& camera, f32 textureFrame,
                         const Texture* hitFlash, TreeModel::Pass pass) const {
    if (!std::isfinite(textureFrame) || textureFrame < 0) {
        return;
    }
    for (const auto& player : m_players) {
        if (player.shown && player.figure) {
            // Portal skin outranks a pending hit flash, just as on the host.
            const Texture* skin = player.hitFlash ? hitFlash : nullptr;
            if (player.portalPhase) {
                skin = m_portalSkin.skinAt(*player.portalPhase);
            }
            const auto& form = player.companions[1];
            if (!form || form->form != static_cast<u32>(PowerupCompanion::Kind::Pojo)) {
                player.figure->drawPose(device, clip, player.placement, lighting,
                                        player.nativePose ? *player.nativePose : player.pose.pose(),
                                        player.action, textureFrame, skin, &camera, pass,
                                        player.weaponHidden, player.armEquipment);
            }
            for (usize slot = 0; slot < player.companions.size(); ++slot) {
                if (const auto& companion = player.companions[slot]) {
                    player.resources.draw(device, slot, *companion, clip, lighting, camera, pass);
                }
            }
        }
    }
    for (const auto& [instance, enemy] : m_enemies) {
        if (enemy.shown && enemy.alpha > 0) {
            resources.drawPose(device, clip, enemy.placement, lighting, enemy.appearance,
                               enemy.nativePose ? *enemy.nativePose : enemy.pose.pose(),
                               enemy.animation.sequence, enemy.animation.frame,
                               enemy.nativeTextureFrame.value_or(textureFrame),
                               enemy.hitFlash ? hitFlash : nullptr, &camera, pass, enemy.alpha,
                               enemy.deathSkin, enemy.dying);
        }
    }
}
void ReplicaActors::drawShadows(RenderDevice& device, const Mat4& clip,
                                const WorldLighting& lighting, const Vec3& eye) const {
    for (const auto& player : m_players) {
        if (player.shown && player.shadow) {
            const auto& shadow = *player.shadow;
            player.figure->drawShadow(device, clip, eye, shadow.ground, shadow.normal, lighting,
                                      shadow.alpha);
        }
    }
}

usize ReplicaActors::visiblePlayers() const {
    return static_cast<usize>(
        std::ranges::count_if(m_players, [](const auto& player) { return player.shown; }));
}
const PlayerFigure* ReplicaActors::playerFigure(u8 seat) const {
    return seat < m_players.size() ? m_players[seat].figure.get() : nullptr;
}
const TreeInfo* ReplicaActors::fireShieldTree(u8 seat) const {
    return seat < m_players.size() ? m_players[seat].resources.fireShieldTree() : nullptr;
}
f32 ReplicaActors::playerScale(u8 seat) const {
    return seat < m_players.size() ? m_players[seat].scale : 1;
}
} // namespace gdl::game
