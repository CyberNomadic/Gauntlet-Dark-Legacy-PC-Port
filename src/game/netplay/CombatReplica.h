#pragma once

#include <map>

#include "engine/net/PacketTransport.h"

#include "game/netplay/CombatSnapshot.h"
#include "game/netplay/SnapshotBlock.h"

namespace gdl::game {

/** Simulation-thread receiver. A complete validated checkpoint replaces the view
 * atomically; a lost fragment leaves the prior roster/health intact. No gameplay,
 * damage, spawning, rewards, saves or animation events execute here. Subsequent
 * full snapshots recover without needing the lost one or a delta baseline. */
class CombatReplica {
public:
    enum class Admission : u8 {
        Committed,
        Pending,
        Duplicate,
        Stale,
        WrongHost,
        WrongEpoch,
        Invalid
    };
    // Header plus the hosted-room envelope fits one 1000-byte provider fragment.
    static constexpr usize kChunkBytes = 960;
    static constexpr usize kHeaderBytes = 36;
    static constexpr usize kPendingSnapshots = 3;
    static constexpr usize kMaxChunks = (SnapshotBlock::kMaxBytes + kChunkBytes - 1) / kChunkBytes;
    static constexpr usize kRepairGroup = 4;
    static constexpr usize kMaxPackets =
        kMaxChunks + (kMaxChunks + kRepairGroup - 1) / kRepairGroup;
    enum class Recovery : u8 { None, SingleLoss };
    using Packets = std::vector<std::vector<u8>>;
    struct Receipt {
        u64 epoch = 0;
        u64 tick = 0;
        u32 checksum = 0;
        bool operator==(const Receipt&) const = default;
    };
    static constexpr usize kReceiptBytes = 32;
    static bool recognizesReceipt(std::span<const u8> bytes);
    static std::optional<std::vector<u8>> encodeReceipt(const Receipt& receipt);
    static std::optional<Receipt> decodeReceipt(std::span<const u8> bytes);

    /** SingleLoss adds one XOR repair packet per four data chunks (except one-chunk
     * checkpoints). Any one missing chunk in each group can be reconstructed. Raw
     * and repair-free modes remain available for diagnostics and reliable delivery. */
    static std::optional<Packets>
    packets(const CombatSnapshot& snapshot,
            SnapshotBlock::Compression compression = SnapshotBlock::Compression::Automatic,
            Recovery recovery = Recovery::None, Receipt* publication = nullptr);
    static std::optional<Packets>
    packets(const CombatPacket::Prepared& snapshot,
            SnapshotBlock::Compression compression = SnapshotBlock::Compression::Automatic,
            Recovery recovery = Recovery::None, Receipt* publication = nullptr);
    /** Only trusted reliable start/travel control may change epoch. */
    bool begin(PacketTransport::Connection host, u64 epoch);
    void clear();
    Admission receive(PacketTransport::Connection sender, std::span<const u8> bytes);
    const CombatSnapshot* latest() const { return m_latest ? &*m_latest : nullptr; }
    /** Available only after complete checksum, decompression and snapshot validation. */
    const std::optional<Receipt>& receipt() const { return m_receipt; }
    usize pending() const { return m_pending.size(); }

private:
    struct Assembly {
        usize bytes = 0;
        u32 checksum = 0;
        std::vector<std::optional<std::vector<u8>>> chunks;
        std::vector<std::optional<std::vector<u8>>> repair;
    };
    PacketTransport::Connection m_host = 0;
    u64 m_epoch = 0;
    std::map<u64, Assembly> m_pending;
    std::optional<CombatSnapshot> m_latest;
    std::optional<Receipt> m_receipt;
};

} // namespace gdl::game
