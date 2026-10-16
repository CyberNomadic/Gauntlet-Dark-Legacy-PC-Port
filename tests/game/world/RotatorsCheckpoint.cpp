#include "game/world/RotatorsCheckpoint.h"

#include <bit>
#include <limits>
#include <utility>

namespace gdl::game {
namespace fc = fixture_checkpoint;
namespace {
auto objectValues(const WorldObject& value) {
    return std::tie(value.name, value.position, value.flags, value.objectFlags, value.next,
                    value.child, value.parent, value.radius, value.noCollision);
}
bool objectValid(const WorldObject& value) {
    return fc::name(value.name) && fc::vector(value.position) && fc::scalar(value.radius) &&
           value.next >= -1 && value.next < 65535 && value.child >= -1 && value.child < 65535 &&
           value.parent >= -1 && value.parent < 65535;
}
u32 parameter(const ItemInstance& instance, usize offset) {
    return static_cast<u32>(instance.params[offset]) |
           (static_cast<u32>(instance.params[offset + 1]) << 8U) |
           (static_cast<u32>(instance.params[offset + 2]) << 16U) |
           (static_cast<u32>(instance.params[offset + 3]) << 24U);
}
bool sameDefinition(const RotatorsCheckpoint::Rotator& saved, const Rotators::Rotator& live,
                    const WorldLayout& layout) {
    return saved.source.matches(layout) && saved.cursor.instance == live.instance &&
           saved.cursor.object == live.object && live.object >= 0 &&
           static_cast<usize>(live.object) < layout.objects().size() &&
           objectValues(saved.target) ==
               objectValues(layout.objects()[static_cast<usize>(live.object)]) &&
           saved.subtype == live.subtype && saved.speed == live.speed &&
           saved.limit == live.limit && saved.origin == live.origin && saved.spot == live.spot &&
           saved.radius == live.radius && saved.height == live.height &&
           saved.pad.has_value() == static_cast<bool>(live.pad);
}
} // namespace

bool RotatorsCheckpoint::Rotator::operator==(const Rotator& other) const {
    return source == other.source && objectValues(target) == objectValues(other.target) &&
           std::tie(cursor, subtype, speed, limit, origin, spot, radius, height, pad) ==
               std::tie(other.cursor, other.subtype, other.speed, other.limit, other.origin,
                        other.spot, other.radius, other.height, other.pad);
}
bool RotatorsCheckpoint::State::valid() const {
    if (rotators.size() > kMaxRotators || (!bound && !rotators.empty())) {
        return false;
    }
    usize details = 0;
    for (usize index = 0; index < rotators.size(); ++index) {
        const auto& saved = rotators[index];
        const auto& source = saved.source;
        if (!source.valid() || !objectValid(saved.target) || !saved.cursor.valid() ||
            source.index != static_cast<usize>(saved.cursor.instance) ||
            (index != 0 && source.index <= rotators[index - 1].source.index) ||
            source.info.type != ItemInfo::kRotator || saved.subtype != source.info.subtype ||
            (saved.subtype != Rotators::kSpinning && saved.subtype != Rotators::kTurnedByPad) ||
            saved.cursor.object != std::bit_cast<s32>(parameter(source.instance, 0)) ||
            saved.speed != std::bit_cast<f32>(parameter(source.instance, 4)) ||
            saved.limit != std::bit_cast<f32>(parameter(source.instance, 8)) ||
            !fc::scalar(saved.speed) || !fc::scalar(saved.limit) ||
            saved.origin != saved.target.position || saved.spot != source.instance.position ||
            saved.radius != source.info.radius || saved.height != source.info.height ||
            (saved.subtype == Rotators::kSpinning &&
             (saved.cursor.started || saved.cursor.done || std::abs(saved.cursor.turned) > kPi)) ||
            (saved.pad &&
             (!saved.pad->valid() || !saved.pad->bound || saved.subtype != Rotators::kTurnedByPad ||
              (source.instance.flags & 2U) != 0))) {
            return false;
        }
        if (source.info.choices.size() > kMaxDetails - details) {
            return false;
        }
        details += source.info.choices.size();
        if (source.instance.collision.size() > kMaxDetails - details) {
            return false;
        }
        details += source.instance.collision.size();
    }
    return true;
}

struct RotatorsCheckpoint::Prepared::Data {
    struct Resource {
        const ItemFigure* figure = nullptr;
        const ItemArchive* archive = nullptr;
        const TreeInfo* tree = nullptr;
    };
    const RotatorsCheckpoint* wrapper = nullptr;
    const Rotators* owner = nullptr;
    const WorldLayout* layout = nullptr;
    const Rotators::Rotator* base = nullptr;
    std::shared_ptr<const u8> lifetime, binding;
    u64 generation = 0;
    State state;
    std::vector<Resource> resources;
    std::vector<std::optional<ItemFigure>> pads;
};
RotatorsCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
RotatorsCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
RotatorsCheckpoint::Prepared&
RotatorsCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
RotatorsCheckpoint::Prepared::~Prepared() = default;

std::optional<RotatorsCheckpoint::State> RotatorsCheckpoint::checkpoint() const {
    State result;
    result.bound = m_owner.m_layout != nullptr;
    for (const auto& live : m_owner.m_sim.rotators) {
        if (live.instance < 0 || live.object < 0 || !m_owner.m_layout ||
            static_cast<usize>(live.object) >= m_owner.m_layout->objects().size()) {
            return std::nullopt;
        }
        const auto source = fc::placement(m_owner.m_layout, static_cast<usize>(live.instance));
        if (!source) {
            return std::nullopt;
        }
        const auto pad = live.pad ? ItemFigureCheckpoint::capture(*live.pad) : std::nullopt;
        if (live.pad && !pad) {
            return std::nullopt;
        }
        result.rotators.push_back(
            {*source,
             m_owner.m_layout->objects()[static_cast<usize>(live.object)],
             {live.instance, live.object, live.turned, live.started, live.done},
             live.subtype,
             live.speed,
             live.limit,
             live.origin,
             live.spot,
             live.radius,
             live.height,
             pad});
    }
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}
std::optional<RotatorsCheckpoint::Prepared>
RotatorsCheckpoint::prepareRestore(const State& state) const {
    if (!state.valid() || state.rotators.size() != m_owner.m_sim.rotators.size() ||
        state.bound != (m_owner.m_layout != nullptr) ||
        m_owner.m_checkpointGeneration == std::numeric_limits<u64>::max()) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->wrapper = this;
    data->owner = &m_owner;
    data->layout = m_owner.m_layout;
    data->base = m_owner.m_sim.rotators.data();
    data->lifetime = m_lifetime;
    data->binding = m_owner.m_checkpointBinding;
    data->generation = m_owner.m_checkpointGeneration;
    data->state = state;
    for (usize index = 0; index < state.rotators.size(); ++index) {
        const auto& saved = state.rotators[index];
        const auto& live = m_owner.m_sim.rotators[index];
        if (!data->layout || !sameDefinition(saved, live, *data->layout)) {
            return std::nullopt;
        }
        std::unique_ptr<ItemFigure> pad;
        Prepared::Data::Resource resource;
        if (saved.pad) {
            pad = ItemFigureCheckpoint::prepare(*live.pad, *saved.pad);
            if (!pad) {
                return std::nullopt;
            }
            const auto presentation = live.pad->presentation();
            resource = {(live.pad ? &*live.pad : nullptr), presentation.archive, presentation.tree};
        }
        data->resources.push_back(resource);
        data->pads.push_back(pad ? std::optional{std::move(*pad)} : std::nullopt);
    }
    Prepared result{std::move(data)};
    return canCommit(result) ? std::optional{std::move(result)} : std::nullopt;
}
bool RotatorsCheckpoint::canCommit(const Prepared& prepared) const {
    if (!prepared.m_data) {
        return false;
    }
    const auto& data = *prepared.m_data;
    if (data.wrapper != this || data.owner != &m_owner || data.lifetime != m_lifetime ||
        data.binding != m_owner.m_checkpointBinding ||
        data.generation != m_owner.m_checkpointGeneration || data.layout != m_owner.m_layout ||
        data.base != m_owner.m_sim.rotators.data() ||
        data.state.rotators.size() != m_owner.m_sim.rotators.size()) {
        return false;
    }
    for (usize index = 0; index < data.state.rotators.size(); ++index) {
        const auto& saved = data.state.rotators[index];
        const auto& live = m_owner.m_sim.rotators[index];
        if (!data.layout || !sameDefinition(saved, live, *data.layout) ||
            (live.pad ? &*live.pad : nullptr) != data.resources[index].figure) {
            return false;
        }
        if (live.pad) {
            const auto current = live.pad->presentation();
            const auto& resource = data.resources[index];
            if (current.archive != resource.archive || current.tree != resource.tree ||
                !resource.archive || !resource.archive->loaded() || !resource.tree) {
                return false;
            }
        }
    }
    return true;
}
bool RotatorsCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    for (usize index = 0; index < data->state.rotators.size(); ++index) {
        const auto& saved = data->state.rotators[index].cursor;
        auto& live = m_owner.m_sim.rotators[index];
        live.turned = saved.turned;
        live.started = saved.started;
        live.done = saved.done;
        live.pad.swap(data->pads[index]);
    }
    ++m_owner.m_checkpointGeneration;
    ++m_owner.m_localEpoch;
    return true;
}
bool RotatorsCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl::game
