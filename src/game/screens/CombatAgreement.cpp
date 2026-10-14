#include "game/screens/CombatAgreement.h"

#include <bit>

#include "engine/io/ByteReader.h"

#include "game/netplay/CanonicalDigestSink.h"
#include "game/netplay/CombatStatePacket.h"
#include "game/netplay/EnemyMissilesCheckpoint.h"
#include "game/netplay/EnemyPoolCheckpoint.h"
#include "game/netplay/GeneratorCheckpoint.h"
#include "game/netplay/PlacedEnemyCheckpoint.h"

namespace gdl::game {
namespace {
void word(CanonicalDigestSink& sink, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        sink.push_back(static_cast<u8>(value >> shift));
    }
}
void real(CanonicalDigestSink& sink, f32 value) {
    word(sink, std::bit_cast<u32>(value));
}
void vector(CanonicalDigestSink& sink, const Vec3& value) {
    real(sink, value.x);
    real(sink, value.y);
    real(sink, value.z);
}
} // namespace
std::optional<CombatAgreement::Digest>
CombatAgreement::state(const OrdinaryCombatCheckpoint::State& state,
                       const StageStatePacket::Catalog& catalog) {
    CanonicalDigestSink sink;
    word(sink, fourcc("AGRE"));
    word(sink, 1); // Local agreement schema, independent of transport framing.
    const auto component = [&](u32 tag, const std::optional<Digest>& digest) {
        if (!digest) {
            return false;
        }
        word(sink, tag);
        sink.append(*digest);
        return true;
    };
    if (!component(1, CombatStatePacket::canonicalDigest(state.combat, state.adapter, true)) ||
        !component(2, GeneratorCheckpoint::canonicalDigest(state.opponents.generators)) ||
        !component(3, EnemyPoolCheckpoint::canonicalDigest(state.opponents.enemies)) ||
        !component(4, EnemyMissilesCheckpoint::canonicalDigest(state.opponents.missiles))) {
        return std::nullopt;
    }
    word(sink, state.opponents.placements.has_value());
    if (state.opponents.placements &&
        !component(5, PlacedEnemyCheckpoint::canonicalDigest(*state.opponents.placements))) {
        return std::nullopt;
    }
    word(sink, state.stage.has_value());
    if (state.stage) {
        if (!catalog.valid()) {
            return std::nullopt;
        }
        sink.append(catalog.fingerprint());
        if (!component(6, StageStatePacket::canonicalDigest(*state.stage, catalog, true))) {
            return std::nullopt;
        }
    }
    return sink.finish();
}
std::optional<CombatAgreement::Digest>
CombatAgreement::context(const OrdinaryCombatTick::Frame& frame, bool nativeCamera) {
    if (!frame.valid()) {
        return std::nullopt;
    }
    CanonicalDigestSink sink;
    word(sink, fourcc("AGRC"));
    word(sink, 1);
    word(sink, static_cast<u32>(frame.ticks));
    real(sink, frame.seconds);
    real(sink, frame.projection.horizontalFov);
    real(sink, frame.projection.aspect);
    word(sink, nativeCamera);
    if (!nativeCamera) {
        vector(sink, frame.camera.position);
        real(sink, frame.camera.pitch);
        real(sink, frame.camera.yaw);
        real(sink, frame.camera.roll);
        vector(sink, frame.attention);
    }
    word(sink, frame.damage.tower);
    real(sink, frame.damage.damageScale);
    word(sink, frame.damage.bossEncounter);
    word(sink, frame.damage.cutscene);
    word(sink, frame.damage.lastRealmLevel);
    word(sink, std::bit_cast<u32>(frame.damage.realm));
    real(sink, frame.damage.playSeconds);
    word(sink, frame.autoMelee);
    return sink.finish();
}
} // namespace gdl::game
