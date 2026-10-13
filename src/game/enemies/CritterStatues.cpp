#include "game/enemies/CritterStatues.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

#include "engine/core/Types.h"

namespace gdl::game {
namespace {
constexpr s32 kContactGridDimension = 64;
constexpr f32 kItemQueryPadding = 15.0f;

bool sameSpawn(const EnemySpawn& a, const EnemySpawn& b) {
    const auto values = [](const EnemySpawn& s) {
        return std::tie(s.zigZagSide, s.kind, s.tier, s.algorithm, s.position, s.direction,
                        s.clearance, s.generator, s.placed, s.patrolBirth, s.allBirthDirections,
                        s.asleep, s.throwInterval, s.priority, s.sight);
    };
    return values(a) == values(b);
}

bool sameInstance(const ItemInstance& a, const ItemInstance& b) {
    return a.info == b.info && a.minPlayers == b.minPlayers && a.flags == b.flags &&
           a.name == b.name && a.position == b.position && a.rotation == b.rotation &&
           a.params == b.params && a.collision.size() == b.collision.size() &&
           std::ranges::equal(a.collision, b.collision, [](const auto& x, const auto& y) {
               return x.normal == y.normal && x.vertices == y.vertices;
           });
}

bool bounded(f32 value, f32 low = -1'000'000, f32 high = 1'000'000) {
    return std::isfinite(value) && value >= low && value <= high;
}
bool bounded(const Vec3& value) {
    return bounded(value.x) && bounded(value.y) && bounded(value.z);
}
bool validName(const std::string& value) {
    return value.size() <= 256 && value.find('\0') == std::string::npos;
}
bool validPlacement(const CritterStatues::Placement& p) {
    const bool death = p.enemy && p.enemy->kind == kDeathKind;
    if ((!death && CritterStatues::treeOf(p.kind).empty()) || !validName(p.form) ||
        !validName(p.instance.name) || !bounded(p.instance.position) ||
        !bounded(p.instance.rotation) || p.instance.collision.size() > 4096 ||
        !bounded(p.radius, 0) || !bounded(p.height, 0) || !bounded(p.viewRadius, 0) ||
        !bounded(p.sight) || p.activeOn < -1'000'000 || p.activeOn > 1'000'000 ||
        (p.carried && *p.carried > 1'000'000)) {
        return false;
    }
    if (p.enemy) {
        const auto& e = *p.enemy;
        if (!death || e.tier < 0 || e.tier > 32767 || e.zigZagSide < -1 || e.zigZagSide > 1 ||
            e.algorithm < -1 || e.algorithm > 32767 || !bounded(e.position) ||
            !bounded(e.direction) || !bounded(e.clearance, 0) || e.generator < -1 ||
            e.generator > 1'000'000 || !bounded(e.throwInterval, 0) || !bounded(e.sight) ||
            e.priority < EnemySpawn::Priority::FreeSlotOnly ||
            e.priority > EnemySpawn::Priority::Visible) {
            return false;
        }
    }
    return std::ranges::all_of(p.instance.collision, [](const auto& triangle) {
        return bounded(triangle.normal) &&
               std::ranges::all_of(triangle.vertices, [](const auto& v) { return bounded(v); });
    });
}
} // namespace

bool CritterStatues::Placement::operator==(const Placement& other) const {
    return kind == other.kind && enemy.has_value() == other.enemy.has_value() &&
           (!enemy || sameSpawn(*enemy, *other.enemy)) && form == other.form &&
           sameInstance(instance, other.instance) && radius == other.radius &&
           height == other.height && viewRadius == other.viewRadius && sight == other.sight &&
           activeOn == other.activeOn && carried == other.carried;
}

bool CritterStatues::Checkpoint::valid() const {
    if (roster.size() > kMaxStatues || statues.size() > roster.size() ||
        risen.size() > roster.size() - statues.size() || nextInstance == 0 ||
        !bounded(contactGridOrigin.x) || !bounded(contactGridOrigin.y) ||
        !bounded(contactGridInverseWidth, 0)) {
        return false;
    }
    usize triangles = 0;
    for (usize i = 0; i < roster.size(); ++i) {
        const auto& definition = roster[i];
        if (definition.instance == 0 || definition.instance >= nextInstance ||
            (i > 0 && definition.instance <= roster[i - 1].instance) ||
            !validPlacement(definition.placement)) {
            return false;
        }
        triangles += definition.placement.instance.collision.size();
        if (triangles > 32768) {
            return false;
        }
    }
    std::vector<bool> used(roster.size(), false);
    usize particles = 0;
    for (usize i = 0; i < statues.size(); ++i) {
        const auto& body = statues[i];
        const auto found =
            std::ranges::lower_bound(roster, body.instance, {}, &Definition::instance);
        if (found == roster.end() || found->instance != body.instance ||
            (i > 0 && body.instance <= statues[i - 1].instance) || !body.figure.valid() ||
            (body.rising && !body.woken) || body.ticksLeft < 0 || body.ticksLeft > 2'000'000 ||
            (!body.rising && body.ticksLeft != 0) ||
            body.figure.index != (body.rising ? kActiveSequence : kIdleSequence) ||
            body.figure.loop == body.rising || body.figure.gateParticles) {
            return false;
        }
        used[static_cast<usize>(found - roster.begin())] = true;
        for (const auto& emitter : body.figure.particles.field.entries) {
            particles += emitter.emitter.particles.size();
            if (particles > 1'000'000) {
                return false;
            }
        }
    }
    // The queue loses the runtime ID natively. Match each pending placement to
    // one removed definition, retaining duplicates and native queue order.
    for (const auto& placement : risen) {
        bool found = false;
        for (usize i = 0; i < roster.size(); ++i) {
            if (!used[i] && placement == roster[i].placement) {
                used[i] = true;
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

std::optional<CritterStatues::Checkpoint> CritterStatues::checkpoint() const {
    Checkpoint state;
    state.roster.reserve(m_prototypes.size());
    for (const auto& prototype : m_prototypes) {
        if (!prototype.figure) {
            return std::nullopt;
        }
        state.roster.push_back(prototype.definition);
    }
    state.statues.reserve(m_statues.size());
    for (const auto& statue : m_statues) {
        const auto figure = statue->figure.checkpoint();
        if (!figure) {
            return std::nullopt;
        }
        state.statues.push_back(
            {statue->instance, *figure, statue->woken, statue->rising, statue->ticksLeft});
    }
    state.risen = m_risen;
    state.contactGridOrigin = m_contactGridOrigin;
    state.contactGridInverseWidth = m_contactGridInverseWidth;
    state.nextInstance = m_nextInstance;
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}

struct CritterStatues::Prepared::Data {
    const CritterStatues* owner = nullptr;
    std::shared_ptr<const u8> lifetime;
    u64 generation = 0;
    std::vector<std::unique_ptr<Statue>> statues;
    std::vector<Placement> risen;
    Vec2 contactGridOrigin{0};
    f32 contactGridInverseWidth = 0;
    u64 nextInstance = 1;
};

CritterStatues::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
CritterStatues::Prepared::Prepared(Prepared&&) noexcept = default;
CritterStatues::Prepared& CritterStatues::Prepared::operator=(Prepared&&) noexcept = default;
CritterStatues::Prepared::~Prepared() = default;

std::optional<CritterStatues::Prepared>
CritterStatues::prepareRestore(const Checkpoint& state) const {
    if (!state.valid() || state.roster.size() != m_prototypes.size()) {
        return std::nullopt;
    }
    for (usize i = 0; i < m_prototypes.size(); ++i) {
        if (state.roster[i] != m_prototypes[i].definition || !m_prototypes[i].figure) {
            return std::nullopt;
        }
    }
    auto data = std::make_unique<Prepared::Data>();
    data->owner = this;
    data->lifetime = m_lifetime;
    data->generation = m_restoreGeneration;
    data->statues.reserve(state.statues.size());
    for (const auto& body : state.statues) {
        const auto found = std::ranges::lower_bound(
            m_prototypes, body.instance, {}, [](const auto& p) { return p.definition.instance; });
        if (found == m_prototypes.end() || found->definition.instance != body.instance ||
            found->archive == nullptr || !found->archive->loaded()) {
            return std::nullopt;
        }
        const auto* active = found->figure->sequenceInfo(kActiveSequence);
        const s32 activeOn = found->definition.placement.activeOn;
        const s32 fullTicks = activeOn > 0        ? activeOn * kTicksPerActiveOn
                              : active != nullptr ? activeTicks(active->frames, active->frameRate)
                                                  : 0;
        if (body.ticksLeft > fullTicks) {
            return std::nullopt;
        }
        auto figure = found->figure->restored(body.figure);
        if (!figure) {
            return std::nullopt;
        }
        auto statue = std::make_unique<Statue>();
        statue->instance = body.instance;
        statue->archive = found->archive;
        statue->placement = found->definition.placement;
        statue->figure = std::move(*figure);
        statue->woken = body.woken;
        statue->rising = body.rising;
        statue->ticksLeft = body.ticksLeft;
        data->statues.push_back(std::move(statue));
    }
    data->risen = state.risen;
    data->contactGridOrigin = state.contactGridOrigin;
    data->contactGridInverseWidth = state.contactGridInverseWidth;
    data->nextInstance = state.nextInstance;
    return Prepared{std::move(data)};
}

bool CritterStatues::canCommit(const Prepared& state) const {
    return state.m_data && state.m_data->owner == this && state.m_data->lifetime == m_lifetime &&
           state.m_data->generation == m_restoreGeneration &&
           std::ranges::all_of(m_prototypes, [](const auto& p) {
               return p.archive != nullptr && p.archive->loaded() && p.figure != nullptr;
           });
}

bool CritterStatues::commitRestore(Prepared&& state) {
    if (!canCommit(state)) {
        return false;
    }
    auto data = std::move(state.m_data);
    m_statues = std::move(data->statues);
    m_risen = std::move(data->risen);
    m_contactGridOrigin = data->contactGridOrigin;
    m_contactGridInverseWidth = data->contactGridInverseWidth;
    m_nextInstance = data->nextInstance;
    ++m_restoreGeneration;
    return true;
}

bool CritterStatues::restore(const Checkpoint& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}
std::string_view CritterStatues::treeOf(CombatantKind kind) {
    switch (kind) {
    case CombatantKind::Golem: return kGolemTree;
    case CombatantKind::Gargoyle: return kGargoyleTree;
    default: return {};
    }
}

s32 CritterStatues::activeTicks(s32 frames, s32 rate) {
    const f32 scale = rate > 0 ? static_cast<f32>(rate) * kFramesPerRateUnit : 1.0f;
    const auto whole = static_cast<s32>(std::floor(0.5f + static_cast<f32>(frames) * scale));
    return whole * kTicksPerActiveOn;
}

bool CritterStatues::add(RenderDevice& device, ItemArchive& archive, const Placement& placement,
                         const WorldCollision* collision) {
    std::string_view tree = treeOf(placement.kind);
    if (placement.enemy && placement.enemy->kind == kDeathKind) {
        tree = placement.enemy->tier == 2 ? "DEATHSTATUE2" : "DEATHSTATUE1";
    }
    if (tree.empty() || m_nextInstance == std::numeric_limits<u64>::max()) {
        return false;
    }
    auto statue = std::make_unique<Statue>();
    statue->placement = placement;
    if (!statue->figure.place(device, archive, tree, placement.instance, collision)) {
        return false;
    }
    statue->figure.play(kIdleSequence, true);
    statue->instance = m_nextInstance++;
    statue->archive = &archive;
    // Keep the initially loaded CPU bindings after a statue rises and is erased.
    // A native figure outside checkpoint bounds still works; only replay declines.
    auto state = statue->figure.checkpoint();
    m_prototypes.push_back({{statue->instance, placement},
                            &archive,
                            state ? statue->figure.restored(*state) : nullptr});
    m_statues.push_back(std::move(statue));
    ++m_restoreGeneration;
    return true;
}

void CritterStatues::clear() {
    m_statues.clear();
    m_prototypes.clear();
    m_risen.clear();
    m_contactGridOrigin = Vec2{0};
    m_contactGridInverseWidth = 0;
    ++m_restoreGeneration;
}

void CritterStatues::setContactGridBounds(const Vec3& minimum, const Vec3& maximum) {
    m_contactGridOrigin = Vec2{minimum.x, minimum.z};
    const f32 cellWidth =
        std::max(maximum.x - minimum.x, maximum.z - minimum.z) / kContactGridDimension;
    m_contactGridInverseWidth = cellWidth > 0 ? static_cast<f32>(1.0 / cellWidth) : 0;
}

s32 CritterStatues::contactCell(f32 coordinate, f32 origin) const {
    return static_cast<s32>(std::clamp((coordinate - origin) * m_contactGridInverseWidth, 0.0f,
                                       static_cast<f32>(kContactGridDimension - 1)));
}

bool CritterStatues::inContactGrid(const Vec3& statue, const Vec3& player, f32 radius) const {
    // StartEnemyGrid (80043490) visits whole cells, not a second sight circle.
    // InitDynGrid (80043A04) receives a 15-unit item margin in every realm.
    const f32 pad = radius > 0 ? kItemQueryPadding + radius : -radius;
    const s32 x = contactCell(statue.x, m_contactGridOrigin.x);
    const s32 z = contactCell(statue.z, m_contactGridOrigin.y);
    return x >= contactCell(player.x - pad, m_contactGridOrigin.x) &&
           x <= contactCell(player.x + pad, m_contactGridOrigin.x) &&
           z >= contactCell(player.z - pad, m_contactGridOrigin.y) &&
           z <= contactCell(player.z + pad, m_contactGridOrigin.y);
}

void CritterStatues::wake(usize index) {
    if (index < m_statues.size()) {
        m_statues[index]->woken = true;
    }
}

std::optional<std::pair<usize, f32>> CritterStatues::nearestAsleep(const Vec3& spot) const {
    std::optional<std::pair<usize, f32>> nearest;
    for (usize i = 0; i < m_statues.size(); ++i) {
        const Statue& statue = *m_statues[i];
        const f32 distance = glm::distance(statue.figure.position(), spot);
        if (!statue.woken && (!nearest.has_value() || distance < nearest->second)) {
            nearest = std::pair{i, distance};
        }
    }
    return nearest;
}

Obstacle CritterStatues::obstacleOf(const Statue& statue) {
    Obstacle cylinder;
    cylinder.centre = statue.figure.position();
    cylinder.yaw = statue.figure.yaw();
    cylinder.cylinderRadius = statue.placement.radius;
    cylinder.halfAcross = statue.placement.radius;
    cylinder.halfAlong = statue.placement.radius;
    cylinder.height = statue.placement.height;
    return cylinder;
}

bool CritterStatues::wakesAt(const Statue& statue, const Vec3& position, f32 radius, f32 height) {
    const Vec3 away = position - statue.figure.position();
    const f32 reach = statue.placement.sight + radius;
    return away.x * away.x + away.z * away.z <= reach * reach &&
           std::abs(away.y) <= statue.placement.height + 0.5f * height;
}

bool CritterStatues::dormantFor(std::span<const Visitor> visitors) const {
    if (!m_risen.empty()) {
        return false;
    }
    for (const auto& statue : m_statues) {
        if (statue->woken || statue->rising) {
            return false;
        }
        const Obstacle cylinder = obstacleOf(*statue);
        for (const auto& visitor : visitors) {
            if (!bounded(visitor.position.x) || !bounded(visitor.position.y) ||
                !bounded(visitor.position.z) || !bounded(visitor.radius, 0) ||
                !bounded(visitor.height, 0)) {
                return false;
            }
            if (!inContactGrid(statue->figure.position(), visitor.position, visitor.radius)) {
                continue;
            }
            if (statue->placement.sight >= 0) {
                if (wakesAt(*statue, visitor.position, visitor.radius, visitor.height)) {
                    return false;
                }
                // Native contact stops here too when a proximity-waking
                // statue is outside sight, before considering its solid body.
                continue;
            }
            if (cylinder.touchedBy(visitor.position, visitor.radius, 0)) {
                return false;
            }
        }
    }
    return true;
}

Vec3 CritterStatues::touch(const Vec3& position, f32 radius, f32 height) {
    Vec3 stood = position;
    for (auto& statue : m_statues) {
        if (!inContactGrid(statue->figure.position(), position, radius)) {
            continue;
        }
        const Obstacle cylinder = obstacleOf(*statue);
        // The wake probe uses placement sight, independently of the solid body's radius.
        if (statue->placement.sight >= 0.0f) {
            if (!wakesAt(*statue, stood, radius, height)) {
                continue;
            }
            statue->woken = true;
        }
        if (cylinder.touchedBy(stood, radius, 0.0f)) {
            stood = cylinder.pushOut(stood, radius);
        }
    }
    return stood;
}

std::vector<Obstacle> CritterStatues::obstacles() const {
    std::vector<Obstacle> out;
    out.reserve(m_statues.size());
    for (const auto& statue : m_statues) {
        out.push_back(obstacleOf(*statue));
    }
    return out;
}

std::vector<MissileTarget> CritterStatues::targets() const {
    std::vector<MissileTarget> out;
    out.reserve(m_statues.size());
    for (usize i = 0; i < m_statues.size(); ++i) {
        const Statue& statue = *m_statues[i];
        out.push_back(MissileTarget{static_cast<s32>(i), statue.figure.position(),
                                    statue.placement.radius, statue.placement.height});
    }
    return out;
}

void CritterStatues::update(s32 ticks, f32 seconds, const Seen& seen) {
    for (auto& statue : m_statues) {
        // Out of view nothing happens to it: neither the waking nor its countdown.
        if (!statue->woken ||
            (seen && !seen(statue->figure.position(), statue->placement.viewRadius))) {
            continue;
        }
        if (!statue->rising) {
            // The ACTIVE sequence starts, and its ticks with it: the record's activeOn, else
            // the sequence's own length (ProcessItems' activetime).
            statue->rising = true;
            statue->figure.play(kActiveSequence, false);
            const TreeSequenceInfo* active = statue->figure.sequenceInfo(kActiveSequence);
            statue->ticksLeft = 0;
            if (statue->placement.activeOn > 0) {
                statue->ticksLeft = statue->placement.activeOn * kTicksPerActiveOn;
            } else if (active != nullptr) {
                statue->ticksLeft = activeTicks(active->frames, active->frameRate);
            }
            continue;
        }
        statue->figure.update(seconds);
        statue->ticksLeft -= ticks;
        if (statue->ticksLeft <= 0) {
            m_risen.push_back(statue->placement);
            statue.reset();
        }
    }
    std::erase_if(m_statues, [](const std::unique_ptr<Statue>& statue) { return !statue; });
}

std::vector<CritterStatues::Placement> CritterStatues::takeRisen() {
    return std::exchange(m_risen, {});
}

void CritterStatues::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                          const CameraFrame* camera) const {
    for (const auto& statue : m_statues) {
        statue->figure.draw(device, clip, lighting, 1.0f, 1.0f, camera);
    }
}
} // namespace gdl::game
