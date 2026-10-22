#include "game/level/RubbleCheckpoint.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace gdl::game {
namespace {
bool matrix(const Mat4& value) {
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            if (!std::isfinite(value[column][row]) || std::abs(value[column][row]) > 1'000'000) {
                return false;
            }
        }
    }
    return value[0].w == 0 && value[1].w == 0 && value[2].w == 0 && value[3].w == 1;
}
} // namespace
bool RubbleCheckpoint::State::valid() const {
    if (pieces.size() > kMaxPieces || resources.size() > kMaxResources) {
        return false;
    }
    for (usize i = 0; i < resources.size(); ++i) {
        const auto& resource = resources[i];
        if (resource.archive >= 16 || resource.object.empty() || resource.object.size() > 64 ||
            resource.object.find('\0') != std::string::npos) {
            return false;
        }
        for (usize j = 0; j < i; ++j) {
            if (resources[j].object == resource.object) {
                return false;
            }
        }
    }
    return std::ranges::all_of(pieces, [&](const auto& piece) {
        return piece.resource < resources.size() && matrix(piece.transform);
    });
}
struct RubbleCheckpoint::Prepared::Data {
    const RubbleCheckpoint* wrapper = nullptr;
    Rubble* owner = nullptr;
    std::shared_ptr<const u8> lifetime;
    std::shared_ptr<const u8> binding;
    u64 generation = 0;
    std::vector<Rubble::Piece> pieces;
};
RubbleCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
RubbleCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
RubbleCheckpoint::Prepared& RubbleCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
RubbleCheckpoint::Prepared::~Prepared() = default;
RubbleCheckpoint::RubbleCheckpoint(Rubble& owner, std::span<ItemArchive* const> archives)
    : m_owner(owner), m_archives(archives.begin(), archives.end()) {}
RubbleCheckpoint::~RubbleCheckpoint() = default;
bool RubbleCheckpoint::bindResources(RenderDevice& device,
                                     std::span<const std::string_view> names) {
    if (m_archives.size() > 16 || names.size() > kMaxResources) {
        return false;
    }
    std::vector<Resource> resources;
    std::vector<std::unique_ptr<Rubble::Piece>> prototypes;
    Rubble prepared;
    for (const auto name : names) {
        if (name.empty() || name.size() > 64 || name.find('\0') != std::string_view::npos ||
            std::ranges::any_of(resources, [&](const auto& row) { return row.object == name; })) {
            return false;
        }
        const auto found = std::ranges::find_if(m_archives, [&](const auto* archive) {
            return archive && archive->models.find(name).has_value();
        });
        if (found == m_archives.end()) {
            continue;
        }
        if (!prepared.leave(device, m_archives, name, Mat4{1})) {
            return false;
        }
        resources.push_back({static_cast<u32>(found - m_archives.begin()), std::string{name}});
        prototypes.push_back(std::make_unique<Rubble::Piece>(std::move(prepared.m_pieces.back())));
        prepared.m_pieces.pop_back();
    }
    m_resources.swap(resources);
    m_prototypes.swap(prototypes);
    m_lifetime = std::make_shared<const u8>(0);
    m_bound = m_owner.m_checkpointBinding;
    return true;
}
std::optional<RubbleCheckpoint::State> RubbleCheckpoint::checkpoint() const {
    if (!m_bound || m_bound != m_owner.m_checkpointBinding || m_owner.size() > kMaxPieces) {
        return std::nullopt;
    }
    State state;
    state.resources = m_resources;
    state.pieces.reserve(m_owner.size());
    for (const auto& piece : m_owner.m_pieces) {
        if (!piece.model) {
            return std::nullopt;
        }
        const auto found = std::ranges::find_if(m_resources, [&](const auto& row) {
            return row.object == piece.object && m_archives[row.archive] == piece.archive;
        });
        if (found == m_resources.end()) {
            return std::nullopt;
        }
        state.pieces.push_back({static_cast<u32>(found - m_resources.begin()), piece.transform});
    }
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}
std::optional<RubbleCheckpoint::Prepared>
RubbleCheckpoint::prepareRestore(const State& state) const {
    if (!m_bound || m_bound != m_owner.m_checkpointBinding || !state.valid() ||
        state.resources != m_resources ||
        m_owner.m_checkpointGeneration == std::numeric_limits<u64>::max()) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->wrapper = this;
    data->owner = &m_owner;
    data->lifetime = m_lifetime;
    data->binding = m_bound;
    data->generation = m_owner.m_checkpointGeneration;
    data->pieces.reserve(state.pieces.size());
    for (const auto& row : state.pieces) {
        auto piece = *m_prototypes[row.resource];
        piece.transform = row.transform;
        data->pieces.push_back(std::move(piece));
    }
    return Prepared{std::move(data)};
}
bool RubbleCheckpoint::canCommit(const Prepared& prepared) const {
    return prepared.m_data && prepared.m_data->wrapper == this &&
           prepared.m_data->owner == &m_owner && prepared.m_data->lifetime == m_lifetime &&
           prepared.m_data->binding == m_bound && m_bound == m_owner.m_checkpointBinding &&
           prepared.m_data->generation == m_owner.m_checkpointGeneration;
}
bool RubbleCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    m_owner.m_pieces.swap(data->pieces);
    ++m_owner.m_checkpointGeneration;
    return true;
}
bool RubbleCheckpoint::restore(const State& state) {
    auto plan = prepareRestore(state);
    return plan && commitRestore(std::move(*plan));
}
} // namespace gdl::game
