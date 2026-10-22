#include "game/camera/TowerCameraCheckpoint.h"

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

#include "game/level/FixtureCheckpointDetail.h"
#include "game/level/LevelWorld.h"

namespace gdl::game {
namespace {
using fixture_checkpoint::scalar;
using fixture_checkpoint::vector;

auto rangeValues(const CameraRange& range) {
    return std::tie(range.radiusMin, range.radiusMax, range.minPitch, range.boundsMin,
                    range.boundsMax);
}
auto cameraValues(const WorldCamera& camera) {
    return std::tie(camera.position, camera.pitch, camera.yaw, camera.roll);
}
bool text(const std::string& value, usize limit) {
    return value.size() <= limit && value.find('\0') == std::string::npos;
}
bool rangeValid(const CameraRange& range) {
    return scalar(range.radiusMin) && range.radiusMin >= 0 && scalar(range.radiusMax) &&
           range.radiusMax >= 0 && scalar(range.minPitch) && vector(range.boundsMin) &&
           vector(range.boundsMax);
}
TowerCameraCheckpoint::Marker markerOf(const WorldLocator& marker) {
    return {marker.delay, marker.next, marker.position, marker.rotation};
}
} // namespace

bool TowerCameraCheckpoint::State::valid() const {
    const auto index = [&](s32 value) {
        return value == -1 || (value >= 0 && static_cast<usize>(value) < markers.size());
    };
    constexpr f32 kTurnRate = kPi / static_cast<f32>(TowerCamera::kTurnSteps);
    if (!text(level.realm, 64) || !text(level.name, 64) || !text(level.title, 256) ||
        !text(level.directory, 512) || !text(level.items, 512) || !text(level.ownItems, 512) ||
        level.realmId < -1 || level.realmId > 255 || level.index < 0 || level.index > 65535 ||
        markers.size() > kMaxMarkers || !rangeValid(range) || !vector(camera.position) ||
        !scalar(camera.pitch) || !scalar(camera.yaw) || camera.roll != 0 || !vector(attention) ||
        !scalar(distance) || distance < 0 || !index(marker) || !index(aimedMarker) ||
        !scalar(yawRate) || std::abs(yawRate) > kTurnRate || !scalar(pitchRate) ||
        std::abs(pitchRate) > kTurnRate || !scalar(turnStep) || turnStep < 0 ||
        turnStep > static_cast<f32>(TowerCamera::kTurnSteps) || ringIndex < 0 ||
        ringIndex >= TowerCamera::kRing) {
        return false;
    }
    // Turned poses are wrapped; reset can retain an authored marker angle or
    // the party's minimum pitch verbatim. Bound those exceptions by the exact
    // trusted definitions, not an arbitrary large network-supplied angle.
    f32 maxYaw = kPi;
    f32 maxPitch = std::max(kPi, std::abs(range.minPitch));
    for (const auto& value : markers) {
        maxYaw = std::max(maxYaw, std::abs(value.rotation.y));
        maxPitch = std::max(maxPitch, std::abs(value.rotation.x));
    }
    if (std::abs(camera.yaw) > maxYaw || std::abs(camera.pitch) > maxPitch) {
        return false;
    }
    return std::ranges::all_of(markers,
                               [](const Marker& value) {
                                   return vector(value.position) && vector(value.rotation);
                               }) &&
           std::ranges::all_of(ringPositions, vector) &&
           std::ranges::all_of(ringDistances,
                               [](f32 value) { return scalar(value) && value >= 0; });
}
bool TowerCameraCheckpoint::State::operator==(const State& other) const {
    return level == other.level && markers == other.markers &&
           rangeValues(range) == rangeValues(other.range) &&
           cameraValues(camera) == cameraValues(other.camera) &&
           std::tie(attention, distance, marker, aimedMarker, yawRate, pitchRate, turnStep,
                    ringPositions, ringDistances, ringIndex) ==
               std::tie(other.attention, other.distance, other.marker, other.aimedMarker,
                        other.yawRate, other.pitchRate, other.turnStep, other.ringPositions,
                        other.ringDistances, other.ringIndex);
}

struct TowerCameraCheckpoint::Prepared::Data {
    const TowerCameraCheckpoint* wrapper = nullptr;
    const TowerCamera* owner = nullptr;
    const LevelWorld* world = nullptr;
    std::shared_ptr<const u8> lifetime, cameraBinding, worldBinding;
    u64 generation = 0;
    State state;
};
TowerCameraCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
TowerCameraCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
TowerCameraCheckpoint::Prepared&
TowerCameraCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
TowerCameraCheckpoint::Prepared::~Prepared() = default;

TowerCameraCheckpoint::TowerCameraCheckpoint(TowerCamera& owner, const LevelWorld& world)
    : m_owner(owner), m_world(world), m_worldBinding(world.m_checkpointBinding) {}
TowerCameraCheckpoint::~TowerCameraCheckpoint() = default;

bool TowerCameraCheckpoint::bound() const {
    return m_world.built() && m_worldBinding == m_world.m_checkpointBinding &&
           m_world.cameraMarkers().size() <= State::kMaxMarkers;
}
bool TowerCameraCheckpoint::binds(const TowerCamera& owner, const LevelWorld& world) const {
    return &owner == &m_owner && &world == &m_world && bound();
}
bool TowerCameraCheckpoint::matches(const State& state) const {
    const auto& markers = m_world.cameraMarkers();
    return bound() && state.level == m_world.ref() &&
           rangeValues(state.range) == rangeValues(m_world.cameraRange()) &&
           state.markers.size() == markers.size() &&
           std::equal(state.markers.begin(), state.markers.end(), markers.begin(),
                      [](const Marker& saved, const WorldLocator& live) {
                          return live.kind == LocatorKind::CameraGame && saved == markerOf(live);
                      });
}
std::optional<TowerCameraCheckpoint::State> TowerCameraCheckpoint::checkpoint() const {
    if (!bound()) {
        return std::nullopt;
    }
    State state;
    state.level = m_world.ref();
    state.range = m_world.cameraRange();
    state.markers.reserve(m_world.cameraMarkers().size());
    for (const auto& marker : m_world.cameraMarkers()) {
        if (marker.kind != LocatorKind::CameraGame) {
            return std::nullopt;
        }
        state.markers.push_back(markerOf(marker));
    }
    state.camera = m_owner.m_state.camera;
    state.attention = m_owner.m_state.attention;
    state.distance = m_owner.m_state.distance;
    state.marker = m_owner.m_state.marker;
    state.aimedMarker = m_owner.m_state.aimedMarker;
    state.yawRate = m_owner.m_state.yawRate;
    state.pitchRate = m_owner.m_state.pitchRate;
    state.turnStep = m_owner.m_state.turnStep;
    state.ringPositions = m_owner.m_state.ringPositions;
    state.ringDistances = m_owner.m_state.ringDistances;
    state.ringIndex = m_owner.m_state.ringIndex;
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}
std::optional<TowerCameraCheckpoint::Prepared>
TowerCameraCheckpoint::prepareRestore(const State& state) const {
    if (!state.valid() || !matches(state) ||
        m_owner.m_checkpointGeneration == std::numeric_limits<u64>::max()) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->wrapper = this;
    data->owner = &m_owner;
    data->world = &m_world;
    data->lifetime = m_lifetime;
    data->cameraBinding = m_owner.m_checkpointBinding;
    data->worldBinding = m_worldBinding;
    data->generation = m_owner.m_checkpointGeneration;
    data->state = state;
    Prepared prepared{std::move(data)};
    return canCommit(prepared) ? std::optional{std::move(prepared)} : std::nullopt;
}
bool TowerCameraCheckpoint::canCommit(const Prepared& prepared) const {
    if (!prepared.m_data) {
        return false;
    }
    const auto& data = *prepared.m_data;
    return data.wrapper == this && data.owner == &m_owner && data.world == &m_world &&
           data.lifetime == m_lifetime && data.cameraBinding == m_owner.m_checkpointBinding &&
           data.worldBinding == m_world.m_checkpointBinding &&
           data.generation == m_owner.m_checkpointGeneration && matches(data.state);
}
bool TowerCameraCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    const auto data = std::move(prepared.m_data);
    const auto& state = data->state;
    m_owner.m_state.camera = state.camera;
    m_owner.m_state.attention = state.attention;
    m_owner.m_state.distance = state.distance;
    m_owner.m_state.marker = state.marker;
    m_owner.m_state.aimedMarker = state.aimedMarker;
    m_owner.m_state.yawRate = state.yawRate;
    m_owner.m_state.pitchRate = state.pitchRate;
    m_owner.m_state.turnStep = state.turnStep;
    m_owner.m_state.ringPositions = state.ringPositions;
    m_owner.m_state.ringDistances = state.ringDistances;
    m_owner.m_state.ringIndex = state.ringIndex;
    ++m_owner.m_checkpointGeneration;
    return true;
}
bool TowerCameraCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl::game
