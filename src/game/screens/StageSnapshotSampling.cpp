#include "game/screens/StageSnapshotSampling.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "game/screens/PlayerRuntime.h"

namespace gdl::game {
namespace {
bool continuous(const CombatAnimation& before, const CombatAnimation& after) {
    return before.generation != 0 && before.generation == after.generation &&
           before.action == after.action && before.sequence == after.sequence &&
           before.frame <= after.frame;
}
bool close(const Mat4& before, const Mat4& after) {
    return glm::distance(Vec3{before[3]}, Vec3{after[3]}) <= 32;
}
template <class Values, class Key, class Projection>
const typename Values::value_type* find(const Values& values, const Key& key,
                                        Projection projection) {
    const auto found = std::ranges::lower_bound(values, key, {}, projection);
    return found != values.end() && std::invoke(projection, *found) == key ? &*found : nullptr;
}
bool posedFixture(FixtureSource source) {
    // These native ItemFigure callers pass presentationAlpha. Switches,
    // generators, rubble, statues and portals intentionally do not.
    return source == FixtureSource::Chest || source == FixtureSource::ChestPreview ||
           source == FixtureSource::Gate || source == FixtureSource::Barrel ||
           source == FixtureSource::Trap;
}
} // namespace

StageSnapshotSampling::Swarm StageSnapshotSampling::captureSwarm(const Enemies& enemies) {
    Swarm result;
    for (s32 slot = 0; slot < Enemies::kMost; ++slot) {
        if (auto anchor = enemies.presentation(slot)) {
            result.enemies.push_back(std::move(*anchor));
        }
    }
    std::ranges::sort(result.enemies, {}, &Enemies::Presentation::instance);
    return result;
}

std::optional<StageSnapshotSampling::Party>
StageSnapshotSampling::captureParty(std::span<const PlayerRuntime> players) {
    if (players.size() > InputCommand::kSeats) {
        return std::nullopt;
    }
    Party result;
    u32 seen = 0;
    for (const auto& runtime : players) {
        const s32 seat = runtime.actor.player();
        if (seat < 0 || seat >= static_cast<s32>(InputCommand::kSeats) ||
            (seen & (1U << static_cast<u32>(seat))) != 0) {
            return std::nullopt;
        }
        seen |= 1U << static_cast<u32>(seat);
        if (runtime.departed || runtime.life == PlayerLife::InTower || !runtime.figure) {
            continue;
        }
        const auto& animator = runtime.figure->animator();
        if (!animator.bound()) {
            return std::nullopt;
        }
        Player value;
        value.life = runtime.life == PlayerLife::Dying ? ReplicaPlayerLife::Dying
                                                      : ReplicaPlayerLife::Standing;
        const auto& animation = animator.player();
        value.animation = {static_cast<u32>(animator.action()), animation.sequence(),
                            animation.generation(), animation.frame(), animation.transition()};
        // These endpoints already include native stance-source blending and
        // generation cuts. In particular, alpha zero still retains the last
        // transitioning pose after the current update finishes its transition.
        animator.evaluatePresentation(value.previous, 0);
        animator.evaluatePresentation(value.current, 1);
        if (!value.animation.valid() || !value.previous.checkpoint() ||
            !value.current.checkpoint()) {
            return std::nullopt;
        }
        result.players[static_cast<usize>(seat)] = std::move(value);
    }
    return result;
}

std::unique_ptr<CombatSnapshot>
StageSnapshotSampling::sample(const CombatSnapshot& previous, const CombatSnapshot& current,
                              f32 blend, EnemyPresentation* presentation, const Swarm* swarm,
                              const Party* party) {
    if (presentation != nullptr) {
        *presentation = {};
    }
    // Reuse the established continuous interpolation without changing its
    // delayed-packet roster policy. Both copies are drawing-only and heap owned.
    const auto mixed = CombatPlayback::samplePair(previous, current, blend);
    auto result = mixed ? CombatPlayback::samplePair(previous, current, 1) : nullptr;
    if (!result) {
        return nullptr;
    }
    EnemyPresentation native;
    native.epoch = current.motion.epoch;
    native.tick = current.motion.tick;
    native.blend = blend;
    native.fromGeometry = previous.geometry;
    native.toGeometry = current.geometry;
    if (previous.motion.cameraContinuity == current.motion.cameraContinuity) {
        result->motion.camera = mixed->motion.camera;
        result->motion.horizontalFov = mixed->motion.horizontalFov;
        result->motion.aspect = mixed->motion.aspect;
    }
    result->worldTextureFrame = mixed->worldTextureFrame;
    if (result->geometry && previous.geometry && mixed->geometry &&
        result->geometry->layout == previous.geometry->layout) {
        // Missing current overrides mean authored defaults, never retained
        // previous visibility. Moving objects are always explicitly captured.
        for (auto& object : result->geometry->objects) {
            const auto* before =
                find(previous.geometry->objects, object.index, &SceneGeometry::Object::index);
            const auto* sampled =
                find(mixed->geometry->objects, object.index, &SceneGeometry::Object::index);
            if (before != nullptr && sampled != nullptr &&
                before->continuity == object.continuity) {
                object.local = sampled->local;
            }
        }
    }
    for (usize seat = 0; seat < result->players.size(); ++seat) {
        auto& after = result->players[seat];
        const auto& before = previous.players[seat];
        const auto& sampled = mixed->players[seat];
        const auto& start = previous.motion.players[seat];
        const auto& end = current.motion.players[seat];
        if (party != nullptr && after && end && after->life != ReplicaPlayerLife::InTower &&
            after->animation.generation != 0) {
            const auto& anchor = party->players[seat];
            if (!anchor || anchor->life != after->life ||
                anchor->animation.action != after->animation.action ||
                anchor->animation.sequence != after->animation.sequence ||
                anchor->animation.generation != after->animation.generation ||
                anchor->animation.frame != after->animation.frame ||
                anchor->animation.transition != after->animation.transition) {
                return nullptr;
            }
            auto pose = anchor->current;
            if (blend < 1) {
                pose.blend(anchor->previous, blend, true);
            }
            auto checkpoint = pose.checkpoint();
            if (!checkpoint) {
                return nullptr;
            }
            native.players[seat] = EnemyPresentation::PlayerPose{
                end->grant, end->continuity, after->life, after->animation, std::move(*checkpoint)};
        }
        if (!after || !before || !sampled || !start || !end || before->life != after->life ||
            start->grant != end->grant || start->continuity != end->continuity) {
            continue;
        }
        result->motion.players[seat] = mixed->motion.players[seat];
        if (party == nullptr && continuous(before->animation, after->animation) &&
            (sampled->animation.transition >= 1 || after->transitionPose)) {
            // A drawing snapshot without native endpoints must not resurrect a
            // completed transition without its exact source. The native party
            // path keeps current scalar bookkeeping and supplies an exact pose.
            after->animation = sampled->animation;
        }
        if (after->shadow && before->shadow) {
            after->shadow = sampled->shadow;
        }
        for (usize slot = 0; slot < after->companions.size(); ++slot) {
            auto& companion = after->companions[slot];
            const auto& from = before->companions[slot];
            const auto& value = sampled->companions[slot];
            if (companion && from && value && from->form == companion->form &&
                continuous(from->animation, companion->animation) &&
                close(from->placement, companion->placement)) {
                companion->placement = value->placement;
                companion->animation = value->animation;
                companion->textureClock = value->textureClock;
            }
        }
    }
    for (auto& enemy : result->enemies) {
        const auto* anchor = swarm == nullptr ? nullptr
                                              : find(swarm->enemies, enemy.instance,
                                                     &Enemies::Presentation::instance);
        if (anchor != nullptr && anchor->kind == static_cast<s32>(enemy.kind) &&
            anchor->tier == static_cast<s32>(enemy.tier) &&
            anchor->variant == static_cast<s32>(enemy.variant)) {
            const f32 phase = std::lerp(anchor->blendStart, anchor->blendEnd, blend);
            auto from = enemy;
            auto to = enemy;
            from.position = phase == 1 ? anchor->position : anchor->previousPosition;
            to.position = anchor->position;
            enemy.position = glm::mix(from.position, to.position, phase);
            enemy.yaw = phase == 1
                            ? anchor->yaw
                            : anchor->previousYaw +
                                  TreePose::wrapAngle(anchor->yaw - anchor->previousYaw) * phase;
            enemy.animation.frame = anchor->animation.frame(phase);
            const auto pose = anchor->animation.pose(phase).checkpoint();
            if (!pose) {
                return nullptr;
            }
            native.enemies.push_back({std::move(from), std::move(to), phase, *pose,
                                      std::lerp(anchor->textureStart, anchor->textureEnd, blend)});
            continue;
        }
        if (swarm != nullptr && enemy.alpha > 0) {
            return nullptr; // visible native owner must never silently lose its anchor
        }
        const auto* from = find(previous.enemies, enemy.instance, &EnemyCombatState::instance);
        const auto* sampled = find(mixed->enemies, enemy.instance, &EnemyCombatState::instance);
        if (from != nullptr && sampled != nullptr && from->kind == enemy.kind &&
            from->tier == enemy.tier && from->variant == enemy.variant &&
            from->life == enemy.life && glm::distance(from->position, enemy.position) <= 32) {
            const auto to = enemy;
            enemy.position = sampled->position;
            enemy.yaw = sampled->yaw;
            if (continuous(from->animation, enemy.animation)) {
                enemy.animation = sampled->animation;
            }
            if (from->animation.action == enemy.animation.action) {
                native.enemies.push_back({*from, to});
            }
        }
    }
    for (auto& shot : result->projectiles) {
        const auto* from = find(previous.projectiles, shot.key(), &ProjectileState::key);
        const auto* sampled = find(mixed->projectiles, shot.key(), &ProjectileState::key);
        if (from == nullptr || sampled == nullptr || from->resource != shot.resource ||
            from->continuity != shot.continuity || from->flags != shot.flags ||
            !close(from->placement, shot.placement)) {
            continue;
        }
        shot.placement = sampled->placement;
        shot.direction = sampled->direction;
        shot.age = sampled->age;
        shot.textureFrame = sampled->textureFrame;
        if (continuous(from->animation, shot.animation)) {
            shot.animation = sampled->animation;
        }
    }
    for (auto& item : result->pickups) {
        const auto* from = find(previous.pickups, item.instance, &PickupState::instance);
        const auto* sampled = find(mixed->pickups, item.instance, &PickupState::instance);
        if (from == nullptr || sampled == nullptr || from->resource != item.resource ||
            from->continuity != item.continuity || !close(from->placement, item.placement)) {
            continue;
        }
        item.placement = sampled->placement;
        item.textureFrame = sampled->textureFrame;
        if (continuous(from->animation, item.animation)) {
            item.animation = sampled->animation;
        }
    }
    for (auto& fixture : result->fixtures) {
        const auto* from = find(previous.fixtures, fixture.key(), &FixtureState::key);
        const auto* sampled = find(mixed->fixtures, fixture.key(), &FixtureState::key);
        if (from == nullptr || sampled == nullptr || from->resource != fixture.resource ||
            from->continuity != fixture.continuity || from->cameraFacing != fixture.cameraFacing ||
            !close(from->placement, fixture.placement)) {
            continue;
        }
        if (posedFixture(fixture.source)) {
            fixture.placement = sampled->placement;
            fixture.textureClock = sampled->textureClock;
            if (continuous(from->pose, fixture.pose)) {
                fixture.pose = sampled->pose;
                fixture.meshFrame = sampled->meshFrame;
                fixture.textureFrame = sampled->textureFrame;
            }
        } else if (fixture.source == FixtureSource::Rock ||
                   fixture.source == FixtureSource::Generator) {
            fixture.textureClock = sampled->textureClock;
        }
    }
    for (auto& fighter : result->fighters) {
        const auto* from = find(previous.fighters, fighter.key(), &FighterMeshState::key);
        const auto* sampled = find(mixed->fighters, fighter.key(), &FighterMeshState::key);
        if (from == nullptr || sampled == nullptr || from->incarnation != fighter.incarnation ||
            from->resource != fighter.resource || from->nodes.size() != fighter.nodes.size() ||
            !close(from->placement, fighter.placement)) {
            continue;
        }
        fighter.placement = sampled->placement;
        fighter.textureClock = sampled->textureClock;
        if ((from->nodes.empty() || from->nodes[0].generation == fighter.nodes[0].generation) &&
            from->sequence == fighter.sequence && from->frame <= fighter.frame) {
            fighter.frame = sampled->frame;
        }
        for (usize i = 0; i < fighter.nodes.size(); ++i) {
            auto& node = fighter.nodes[i];
            if (from->nodes[i].generation == node.generation &&
                from->nodes[i].sequence == node.sequence && from->nodes[i].frame <= node.frame) {
                node.transform = sampled->nodes[i].transform;
                node.frame = sampled->nodes[i].frame;
            }
        }
    }
    native.shownGeometry = result->geometry;
    if (!result->valid() || !native.matches(*result)) {
        return nullptr;
    }
    if (presentation != nullptr) {
        *presentation = std::move(native);
    }
    return result;
}
} // namespace gdl::game
