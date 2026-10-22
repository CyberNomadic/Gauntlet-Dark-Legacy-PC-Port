#pragma once

#include <array>

#include "game/level/Rubble.h"

namespace gdl::game {
/** Append-only debris is restored by value using immutable prebound native
 * meshes. Archives and their ordered lookup list must outlive this wrapper.
 * No gameplay output or graphics upload occurs during prepare/commit. */
class RubbleCheckpoint {
public:
    static constexpr usize kMaxPieces = 4096;
    static constexpr usize kMaxResources = 256;
    static constexpr std::array<std::string_view, 5> kNativeObjects{
        Rubble::kItem, Rubble::kChest, Rubble::kSilverChest, Rubble::kBlownBarrel,
        Rubble::kGasBarrel};
    struct Resource {
        u32 archive = 0;
        std::string object;
        bool operator==(const Resource&) const = default;
    };
    struct Piece {
        u32 resource = 0;
        Mat4 transform{1};
        bool operator==(const Piece&) const = default;
    };
    struct State {
        std::vector<Resource> resources;
        std::vector<Piece> pieces;
        bool valid() const;
        bool operator==(const State&) const = default;
    };
    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class RubbleCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };
    RubbleCheckpoint(Rubble& owner, std::span<ItemArchive* const> archives);
    ~RubbleCheckpoint();
    RubbleCheckpoint(const RubbleCheckpoint&) = delete;
    RubbleCheckpoint& operator=(const RubbleCheckpoint&) = delete;
    RubbleCheckpoint(RubbleCheckpoint&&) = delete;
    RubbleCheckpoint& operator=(RubbleCheckpoint&&) = delete;
    /** Load-time operation. Missing native debris meshes are legal; any live
     * uncatalogued mesh makes the complete checkpoint fail, never disappear. */
    bool bindResources(RenderDevice& device,
                       std::span<const std::string_view> names = kNativeObjects);
    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);

private:
    Rubble& m_owner;
    std::vector<ItemArchive*> m_archives;
    std::vector<Resource> m_resources;
    std::vector<std::unique_ptr<Rubble::Piece>> m_prototypes;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
    std::shared_ptr<const u8> m_bound;
};
} // namespace gdl::game
