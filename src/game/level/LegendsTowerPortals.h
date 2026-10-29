#pragma once

#include <span>
#include <string_view>

#include "game/level/LegendsPortals.h"

namespace gdl::game {

enum class LegendsTowerPortalKind { Shards, Runestones };

struct LegendsTowerPortalDefinition {
    Vec3 position{0};
    f32 radius = 0;
    std::string_view map;
};

/** Tower.bin's scripted completion exits are not IPR transporter fixtures.
 * Their contact test uses player centres in XZ, without body radii or height.
 * This class emits travel once; player departure and presentation belong to
 * the scene. Call with simulation time, not interpolated presentation time. */
class LegendsTowerPortalMotion {
public:
    explicit LegendsTowerPortalMotion(LegendsTowerPortalKind kind);
    bool advance(f32 seconds, std::span<const LegendsPortalVisitor> visitors, bool enabled);

    const LegendsTowerPortalDefinition& definition() const { return m_definition; }
    f32 charge() const { return static_cast<f32>(m_charge); }
    f32 dwell() const { return static_cast<f32>(m_dwell); }
    u8 occupied() const { return m_occupied; }
    bool departed() const { return m_departed; }

private:
    LegendsTowerPortalKind m_kind;
    LegendsTowerPortalDefinition m_definition;
    f64 m_charge = 0;
    f64 m_dwell = 0;
    u8 m_occupied = 0;
    bool m_departed = false;
};

} // namespace gdl::game
