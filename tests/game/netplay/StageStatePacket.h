#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "game/screens/LevelFixturesCheckpoint.h"
#include "game/world/ExitPortalsCheckpoint.h"
#include "game/world/LevelTransportersCheckpoint.h"
#include "game/world/LevelWorldCheckpoint.h"
#include "game/world/TowerCameraCheckpoint.h"

namespace gdl::game {
/** Complete mutable stage seed. Immutable collision surfaces are resolved only
 * through an explicitly trusted load-time catalog, never created from a packet.
 * Native owner prepareRestore remains the final resource/binding authority. */
class StageStatePacket {
public:
    struct State {
        LevelWorldCheckpoint::State world;
        LevelFixturesCheckpoint::State fixtures;
        std::optional<TowerCameraCheckpoint::State> camera;
        std::optional<ExitPortalsCheckpoint::State> portals;
        std::optional<LevelTransportersCheckpoint::State> transporters;
        bool operator==(const State&) const = default;
    };
    static constexpr u32 kVersion = 7;
    static constexpr usize kHeaderBytes = 56;
    static constexpr usize kMaxBytes = 512 * 1024;
    static constexpr usize kMaxDecodedBytes = 4 * 1024 * 1024;
    static constexpr usize kMaxRecords = 65535;
    static constexpr usize kMaxString = 4096;
    static constexpr usize kMaxStringBytes = 64 * 1024;
    static constexpr usize kMaxCatalogTriangles = 65535;
    // Fast avoids a second compression pass unless the plain candidate exceeds
    // the wire budget. Automatic compares both lossless candidates for tooling.
    // All compressed policies fall back to raw when it is smaller.
    enum class Encoding { Automatic, Raw, Plain, Fast };

    class Catalog {
    public:
        Catalog() = default;
        /** Explicit load-time operation on a locally captured, trusted stage.
         * No resource lookup, graphics upload, or live owner mutation. */
        static std::optional<Catalog> build(const State& trusted);
        /** Read-only diagnostics retain every authored particle emitter. This
         * does not expand packet encoding or native restore admission. */
        static std::optional<Catalog> buildObservation(const State& trusted);
        const std::array<u8, 32>& fingerprint() const;
        bool valid() const;

    private:
        friend class StageStatePacket;
        struct Data;
        static std::optional<Catalog> build(const State& trusted, bool observation);
        explicit Catalog(std::shared_ptr<const Data> data) : m_data(std::move(data)) {}
        std::shared_ptr<const Data> m_data;
    };
    static std::optional<std::vector<u8>> encode(const State& state, const Catalog& catalog,
                                                 Encoding encoding = Encoding::Fast);
    /** SHA-256 of the uncompressed canonical body, not the compressed envelope.
     * Optional normalization elides draw-overwritten poses/point-light lists.
     * Not validation: the original state must still pass native preparation. */
    static std::optional<std::array<u8, 32>> canonicalDigest(const State& state,
                                                             const Catalog& catalog,
                                                             bool presentationIndependent = false);
    static std::optional<State> decode(std::span<const u8> bytes, const Catalog& catalog);
    /** Actual on-wire size, including the header. Performs encoding; a refused
     * packet has no encoded size, even if its uncompressed body is valid. */
    static std::optional<usize> encodedSize(const State& state, const Catalog& catalog,
                                            Encoding encoding = Encoding::Fast);
    /** Complete uncompressed body size, excluding the header. Bounded by the
     * decoded-work limit, independently of compression and the wire limit. */
    static std::optional<usize> decodedSize(const State& state, const Catalog& catalog);
    /** Header/length/version/codec budgets only: opaque outer transport may use this without
     * claiming payload validity or accepting an unauthenticated resource catalog. */
    static bool envelopeValid(std::span<const u8> bytes);
};
} // namespace gdl::game
