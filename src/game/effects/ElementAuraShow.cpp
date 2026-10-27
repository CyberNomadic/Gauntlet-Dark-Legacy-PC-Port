#include "game/effects/ElementAuraShow.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace gdl::game {

namespace {

using Body = ElementAuraShow::Body;
using Kind = ElementAuraShow::Kind;
using Sprite = ElementAura::Sprite;

constexpr f32 kPi = std::numbers::pi_v<f32>;
constexpr f32 kDrop = -22.0f;    ///< the drips' fall, units a second squared
constexpr f32 kEmberLift = 4.0f; ///< the embers' lift, units a second squared
constexpr f32 kRayRise = 0.3f;   ///< seconds a ray takes to reach its full length
constexpr s32 kFire = 1;
constexpr s32 kElectric = 2;
constexpr s32 kHoly = 3;
constexpr s32 kAcid = 4;

u32 nextRandom(u32& state) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}

/** A share of one, never one itself. */
f32 unit(u32& state) {
    return static_cast<f32>(nextRandom(state) >> 8U) / static_cast<f32>(1U << 24U);
}

f32 between(u32& state, f32 low, f32 high) {
    return low + (high - low) * unit(state);
}

f32 ease(f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/** A colour from shares of one, premultiplied so it fades alike under every blend. */
Color tone(f32 r, f32 g, f32 b, f32 a) {
    a = std::clamp(a, 0.0f, 1.0f);
    return Color::fromFloats(r * a, g * a, b * a, a);
}

Color toneMix(const Vec3& from, const Vec3& to, f32 t, f32 a) {
    const Vec3 c = glm::mix(from, to, std::clamp(t, 0.0f, 1.0f));
    return tone(c.x, c.y, c.z, a);
}

/** How much larger than the swarm's usual body this one is. */
f32 scaleOf(const Body& body) {
    return body.radius / ElementAuraShow::kBodyRadius;
}

/** The shell the pieces are born on: an ellipsoid a little wider than the body. */
Vec3 axesOf(const Body& body) {
    return {1.1f * body.radius, 0.52f * body.height, 1.1f * body.radius};
}

Vec3 centreOf(const Body& body) {
    return {0.0f, 0.5f * body.height, 0.0f};
}

/** A direction out of the shell with its height share between `low` and `high`. */
Vec3 shellDirection(u32& random, f32 low, f32 high) {
    const f32 y = between(random, low, high);
    const f32 angle = between(random, 0.0f, 2.0f * kPi);
    const f32 ring = std::sqrt(std::max(1.0f - y * y, 0.0f));
    return {std::cos(angle) * ring, y, std::sin(angle) * ring};
}

/** The point of the shell along `direction`, `push` of the way out from the centre. */
Vec3 onShell(const Body& body, const Vec3& direction, f32 push) {
    return centreOf(body) + direction * axesOf(body) * push;
}

Vec3 anyDirection(u32& random) {
    const Vec3 raw{between(random, -1.0f, 1.0f), between(random, -1.0f, 1.0f),
                   between(random, -1.0f, 1.0f)};
    const f32 length = glm::length(raw);
    return length > 1.0e-3f ? raw / length : Vec3{0.0f, 1.0f, 0.0f};
}

/** A camera-facing quad about `centre`, `dx` to its right edge and `dy` to its top. */
void quad(ImmediateBatch& batch, const Vec3& centre, const Vec3& dx, const Vec3& dy, Color color,
          const ElementAura::Cell& cell) {
    batch.vertex(centre - dx + dy, color, Vec2{cell.u0, cell.v0});
    batch.vertex(centre + dx + dy, color, Vec2{cell.u1, cell.v0});
    batch.vertex(centre + dx - dy, color, Vec2{cell.u1, cell.v1});
    batch.vertex(centre - dx + dy, color, Vec2{cell.u0, cell.v0});
    batch.vertex(centre + dx - dy, color, Vec2{cell.u1, cell.v1});
    batch.vertex(centre - dx - dy, color, Vec2{cell.u0, cell.v1});
}

/** A strip from `from` to `to`, `across` to either edge, u along it. */
void strip(ImmediateBatch& batch, const Vec3& from, const Vec3& to, const Vec3& across,
           Color start, Color end, const ElementAura::Cell& cell) {
    batch.vertex(from - across, start, Vec2{cell.u0, cell.v0});
    batch.vertex(to - across, end, Vec2{cell.u1, cell.v0});
    batch.vertex(to + across, end, Vec2{cell.u1, cell.v1});
    batch.vertex(from - across, start, Vec2{cell.u0, cell.v0});
    batch.vertex(to + across, end, Vec2{cell.u1, cell.v1});
    batch.vertex(from + across, start, Vec2{cell.u0, cell.v1});
}

/** The half-width vector of a strip along `along`, facing the camera. */
Vec3 acrossOf(const Vec3& along, const Vec3& forward, const Vec3& fallback, f32 half) {
    const Vec3 normal = glm::cross(along, forward);
    const f32 length = glm::length(normal);
    return (length > 1.0e-4f ? normal / length : fallback) * half;
}

} // namespace

void ElementAuraShow::step(std::span<const Body> bodies, f32 seconds) {
    seconds = std::clamp(seconds, 0.0f, 0.25f);
    for (Aura& aura : m_auras) {
        aura.seen = false;
    }
    for (const Body& body : bodies) {
        if (body.element < kFire || body.element > kAcid || body.instance == 0) {
            continue;
        }
        const auto found = std::ranges::find_if(
            m_auras, [&](const Aura& aura) { return aura.body.instance == body.instance; });
        if (found != m_auras.end()) {
            found->body = body;
            found->seen = true;
            continue;
        }
        if (m_auras.size() >= kMostAuras) {
            continue;
        }
        Aura aura;
        aura.body = body;
        aura.random = static_cast<u32>(body.instance * 2654435761ULL) | 1U;
        aura.seen = true;
        m_auras.push_back(std::move(aura));
    }
    std::erase_if(m_auras, [](const Aura& aura) { return !aura.seen; });
    for (Aura& aura : m_auras) {
        advance(aura, seconds);
    }
}

void ElementAuraShow::owe(Aura& aura, usize lane, f32 perSecond, Kind kind, f32 seconds) {
    aura.owed[lane] += perSecond * seconds;
    while (aura.owed[lane] >= 1.0f) {
        bear(aura, kind);
        aura.owed[lane] -= 1.0f;
    }
}

void ElementAuraShow::advance(Aura& aura, f32 seconds) {
    aura.age += seconds;
    const Body& body = aura.body;
    const f32 scale = scaleOf(body);
    u32& random = aura.random;
    switch (body.element) {
    case kFire:
        owe(aura, 0, 16.0f, Kind::Tongue, seconds);
        owe(aura, 1, 12.0f, Kind::Ember, seconds);
        owe(aura, 2, 2.5f, Kind::Smoke, seconds);
        break;
    case kElectric:
        // Static crackles all over; the arcs come in bursts, a few at a time.
        owe(aura, 0, 36.0f, Kind::Static, seconds);
        aura.nextBurst -= seconds;
        while (aura.nextBurst <= 0.0f) {
            const u32 arcs = 1 + (unit(random) < 0.45f ? 1U : 0U) + (unit(random) < 0.2f ? 1U : 0U);
            for (u32 i = 0; i < arcs; ++i) {
                bearArc(aura);
            }
            aura.nextBurst += between(random, 0.04f, 0.16f);
        }
        break;
    case kHoly:
        owe(aura, 0, 9.0f, Kind::Ray, seconds);
        owe(aura, 1, 6.0f, Kind::Glint, seconds);
        owe(aura, 2, 4.0f, Kind::Mote, seconds);
        break;
    default: // acid: the haze rolls out over the ground in rings
        owe(aura, 0, 9.0f, Kind::Wisp, seconds);
        owe(aura, 1, 3.5f, Kind::Drip, seconds);
        aura.nextBurst -= seconds;
        while (aura.nextBurst <= 0.0f) {
            bearRing(aura, Vec3{0.0f, kRingLift, 0.0f}, 0.9f * body.radius, 3.4f * body.radius,
                     1.0f);
            aura.nextBurst += 0.42f;
        }
        break;
    }
    std::vector<Vec3> splashes;
    for (Piece& piece : aura.pieces) {
        piece.age += seconds;
        // A drip that reaches the ground splashes there and is gone.
        if (piece.kind == Kind::Drip && piece.age < piece.life) {
            const Vec3 at = placeOf(body, piece) - body.feet;
            if (at.y <= 0.05f) {
                splashes.push_back(at);
                piece.age = piece.life;
            }
        }
    }
    std::erase_if(aura.pieces, [](const Piece& piece) { return piece.age >= piece.life; });
    for (const Vec3& at : splashes) {
        bearRing(aura, Vec3{at.x, kRingLift, at.z}, 0.25f * scale, 0.9f * scale, 0.3f);
    }
}

void ElementAuraShow::bear(Aura& aura, Kind kind) {
    if (aura.pieces.size() >= kMostPieces) {
        return;
    }
    const Body& body = aura.body;
    const f32 scale = scaleOf(body);
    u32& random = aura.random;
    Piece piece;
    piece.kind = kind;
    piece.phase = between(random, 0.0f, 2.0f * kPi);
    const f32 lot = unit(random);
    switch (kind) {
    case Kind::Tongue: { // a flame licks up off the skin, swaying
        const Vec3 direction = shellDirection(random, -0.2f, 0.85f);
        piece.life = between(random, 0.45f, 0.7f);
        piece.size = (1.3f + 0.7f * lot) * scale;
        piece.origin = onShell(body, direction, 0.9f);
        piece.velocity = Vec3{0.0f, (4.5f + 2.0f * lot) * scale, 0.0f} +
                         Vec3{direction.x, 0.0f, direction.z} * 0.6f * scale;
        piece.frame = static_cast<u8>(nextRandom(random) % ElementAura::kFlames);
        break;
    }
    case Kind::Ember: { // a spark flies up and away, lifting as it goes
        const Vec3 direction = shellDirection(random, -0.3f, 0.9f);
        piece.life = between(random, 0.5f, 0.9f);
        piece.size = (0.25f + 0.2f * lot) * scale;
        piece.origin = onShell(body, direction, 0.95f);
        piece.velocity = Vec3{direction.x * 2.0f, 4.0f + 3.0f * lot, direction.z * 2.0f} * scale;
        break;
    }
    case Kind::Smoke: { // a puff off the top, drifting up and growing
        const Vec3 direction = shellDirection(random, 0.75f, 1.0f);
        piece.life = between(random, 1.1f, 1.5f);
        piece.size = 1.0f * scale;
        piece.origin = onShell(body, direction, 0.8f);
        piece.velocity = Vec3{direction.x * 0.5f, 2.2f, direction.z * 0.5f} * scale;
        break;
    }
    case Kind::Static: // a crackle on the skin, there and gone
        piece.life = between(random, 0.04f, 0.09f);
        piece.size = (0.18f + 0.12f * lot) * scale;
        piece.origin = onShell(body, shellDirection(random, -0.9f, 0.95f), 1.02f);
        break;
    case Kind::Wisp: { // vapour rising slowly off the body
        const Vec3 direction = shellDirection(random, -0.6f, 0.8f);
        piece.life = between(random, 1.2f, 1.6f);
        piece.size = (0.6f + 0.3f * lot) * scale;
        piece.origin = onShell(body, direction, 1.0f);
        piece.velocity = Vec3{0.0f, (1.0f + 0.6f * lot) * scale, 0.0f};
        break;
    }
    case Kind::Drip: // a drop runs off and falls to the ground
        piece.life = 1.5f;
        piece.size = 0.35f * scale;
        piece.origin = onShell(body, shellDirection(random, -0.2f, 0.6f), 1.0f);
        piece.velocity = Vec3{0.0f, -0.5f * scale, 0.0f};
        break;
    case Kind::Ray: { // light breaks out from within, any way at all
        const Vec3 axes = axesOf(body);
        piece.life = 0.9f;
        piece.origin = centreOf(body) + Vec3{between(random, -0.45f, 0.45f) * axes.x,
                                             between(random, -0.55f, 0.55f) * axes.y,
                                             between(random, -0.45f, 0.45f) * axes.z};
        piece.velocity = anyDirection(random);
        if (piece.velocity.y < -0.15f) { // never down into the floor
            piece.velocity.y = -piece.velocity.y;
        }
        piece.size = (1.8f + 2.2f * lot) * scale;
        break;
    }
    case Kind::Glint: // a star on the skin, swelling and gone
        piece.life = between(random, 0.22f, 0.3f);
        piece.size = (0.5f + 0.4f * lot) * scale;
        piece.origin = onShell(body, shellDirection(random, -0.8f, 0.95f), 1.05f);
        break;
    case Kind::Mote: // a mote drifting up
        piece.life = 1.4f;
        piece.size = 0.2f * scale;
        piece.origin = onShell(body, shellDirection(random, -0.5f, 0.9f), 0.9f);
        piece.velocity = Vec3{0.0f, 1.5f * scale, 0.0f};
        break;
    default: // arcs and rings are borne their own way
        return;
    }
    aura.pieces.push_back(piece);
}

void ElementAuraShow::bearArc(Aura& aura) {
    if (aura.pieces.size() >= kMostPieces) {
        return;
    }
    const Body& body = aura.body;
    const f32 scale = scaleOf(body);
    u32& random = aura.random;
    Piece piece;
    piece.kind = Kind::Arc;
    piece.life = between(random, 0.06f, 0.13f);
    piece.size = 0.12f * scale;
    // From one point of the shell to another a turn away, jagging over the skin between.
    const Vec3 from = shellDirection(random, -0.9f, 0.95f);
    const Vec3 swing{between(random, -1.0f, 1.0f), between(random, -0.8f, 0.8f),
                     between(random, -1.0f, 1.0f)};
    const Vec3 to = glm::normalize(from + swing * between(random, 0.5f, 1.1f));
    const u8 count = static_cast<u8>(6 + nextRandom(random) % 3);
    for (u8 i = 0; i < count; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(count - 1);
        const Vec3 direction = glm::normalize(glm::mix(from, to, t));
        Vec3 point = onShell(body, direction, 1.04f + 0.06f * unit(random));
        if (i > 0 && i + 1 < count) {
            const f32 jag = 0.22f * scale * (1.0f - std::abs(2.0f * t - 1.0f));
            point += Vec3{between(random, -1.0f, 1.0f), between(random, -1.0f, 1.0f),
                          between(random, -1.0f, 1.0f)} *
                     jag;
        }
        piece.path[i] = point;
    }
    piece.points = count;
    aura.pieces.push_back(piece);
}

void ElementAuraShow::bearRing(Aura& aura, const Vec3& at, f32 from, f32 to, f32 life) {
    if (aura.pieces.size() >= kMostPieces) {
        return;
    }
    Piece piece;
    piece.kind = Kind::Ring;
    piece.life = life;
    piece.origin = at;
    piece.size = from;
    piece.phase = to;
    aura.pieces.push_back(piece);
}

Vec3 ElementAuraShow::placeOf(const Body& body, const Piece& piece) {
    const f32 scale = scaleOf(body);
    const f32 age = piece.age;
    Vec3 local = piece.origin + piece.velocity * age;
    switch (piece.kind) {
    case Kind::Tongue:
        local.x += std::sin(age * 7.0f + piece.phase) * 0.15f * scale;
        break;
    case Kind::Ember:
        local.y += 0.5f * kEmberLift * age * age;
        local.x += std::sin(age * 9.0f + piece.phase) * 0.3f * scale;
        break;
    case Kind::Smoke:
        local.x += std::sin(age * 1.3f + piece.phase) * 0.4f * scale;
        break;
    case Kind::Wisp:
        local.x += std::sin(age * 2.2f + piece.phase) * 0.35f * scale;
        local.z += std::cos(age * 1.7f + piece.phase) * 0.35f * scale;
        break;
    case Kind::Drip:
        local.y += 0.5f * kDrop * age * age;
        break;
    case Kind::Mote:
        local.x += std::sin(age * 3.0f + piece.phase) * 0.2f * scale;
        break;
    case Kind::Ray:
        local = piece.origin;
        break;
    default:
        break;
    }
    return body.feet + local;
}

void ElementAuraShow::draw(RenderDevice& device, const Mat4& clip, const CameraFrame& camera) {
    if (m_auras.empty()) {
        return;
    }
    const Texture* atlas = m_atlas.texture(device);
    if (atlas == nullptr) {
        return;
    }
    Frame frame;
    frame.right = camera.right;
    frame.up = camera.up;
    frame.forward = camera.forward;
    const Vec3 flat{camera.right.x, 0.0f, camera.right.z};
    const f32 flatLength = glm::length(flat);
    frame.upright = flatLength > 1.0e-4f ? flat / flatLength : Vec3{1.0f, 0.0f, 0.0f};
    m_additive.clear();
    m_additive.begin(PrimitiveTopology::TriangleList);
    m_alpha.clear();
    m_alpha.begin(PrimitiveTopology::TriangleList);
    for (const Aura& aura : m_auras) {
        drawAura(aura, frame);
    }
    m_additive.end();
    m_alpha.end();
    DrawState soft;
    soft.blend = BlendMode::Alpha;
    soft.depthWrite = false;
    soft.alphaTest = DrawState::kTranslucentAlphaTest;
    if (!m_alpha.empty()) {
        device.draw(m_alpha, *atlas, clip, soft); // the smoke, behind the flames
    }
    DrawState lit = soft;
    lit.blend = BlendMode::Additive;
    if (!m_additive.empty()) {
        device.draw(m_additive, *atlas, clip, lit);
    }
}

void ElementAuraShow::drawAura(const Aura& aura, const Frame& frame) {
    const Body& body = aura.body;
    const f32 scale = scaleOf(body);
    const Vec3 centre = body.feet + centreOf(body);
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const auto glowCell = ElementAura::cell(Sprite::Glow);
    const auto ringCell = ElementAura::cell(Sprite::Ring);
    const auto beamCell = ElementAura::cell(Sprite::Beam);
    const auto streakCell = ElementAura::cell(Sprite::Streak);
    const auto glintCell = ElementAura::cell(Sprite::Glint);
    const auto dropCell = ElementAura::cell(Sprite::Drop);
    // The body's own light first: a haze in its element's colour about it.
    switch (body.element) {
    case kFire: {
        const f32 flicker = 0.5f * (std::sin(aura.age * 23.0f) + std::sin(aura.age * 37.0f));
        quad(m_additive, centre, frame.right * (1.4f * body.radius), frame.up * (0.62f * body.height),
             tone(1.0f, 0.5f, 0.15f, 0.2f + 0.05f * flicker), glowCell);
        break;
    }
    case kElectric: {
        const auto arcs = static_cast<usize>(std::ranges::count_if(
            aura.pieces, [](const Piece& piece) { return piece.kind == Kind::Arc; }));
        quad(m_additive, centre, frame.right * (1.3f * body.radius), frame.up * (0.6f * body.height),
             tone(0.45f, 0.6f, 1.0f, 0.07f + 0.06f * static_cast<f32>(std::min<usize>(arcs, 3))),
             glowCell);
        break;
    }
    case kHoly:
        quad(m_additive, centre, frame.right * (1.2f * body.radius), frame.up * (0.55f * body.height),
             tone(1.0f, 0.97f, 0.8f, 0.2f + 0.08f * std::sin(aura.age * 3.0f)), glowCell);
        break;
    default: // acid: an inner haze breathing inside a wider, fainter one
        quad(m_additive, centre, frame.right * (1.5f * body.radius), frame.up * (0.65f * body.height),
             tone(0.35f, 1.0f, 0.3f, 0.28f + 0.07f * std::sin(aura.age * 2.5f)), glowCell);
        quad(m_additive, centre, frame.right * (2.2f * body.radius), frame.up * (0.9f * body.height),
             tone(0.3f, 1.0f, 0.25f, 0.1f), glowCell);
        break;
    }
    for (const Piece& piece : aura.pieces) {
        const f32 t = std::clamp(piece.age / piece.life, 0.0f, 1.0f);
        const Vec3 at = placeOf(body, piece);
        switch (piece.kind) {
        case Kind::Tongue: { // standing up off its base, flickering through the frames
            const f32 width = piece.size * (1.0f - 0.45f * t);
            const f32 height = 2.2f * width;
            const u32 frameNow = piece.frame + static_cast<u32>(piece.age / 0.07f);
            quad(m_additive, at + up * (0.5f * height), frame.upright * (0.5f * width),
                 up * (0.5f * height),
                 toneMix(Vec3{1.0f, 1.0f, 0.85f}, Vec3{1.0f, 0.45f, 0.2f}, t,
                         t < 0.65f ? 1.0f : (1.0f - t) / 0.35f),
                 ElementAura::cell(ElementAura::flame(frameNow)));
            break;
        }
        case Kind::Ember: {
            const f32 half = 0.5f * piece.size * (1.0f - 0.6f * t);
            quad(m_additive, at, frame.right * half, frame.up * half,
                 toneMix(Vec3{1.0f, 0.8f, 0.3f}, Vec3{1.0f, 0.25f, 0.0f}, t,
                         t < 0.6f ? 1.0f : (1.0f - t) / 0.4f),
                 glowCell);
            break;
        }
        case Kind::Smoke: {
            const f32 half = 0.5f * piece.size * (1.0f + 1.6f * t);
            quad(m_alpha, at, frame.right * half, frame.up * half,
                 tone(0.25f, 0.22f, 0.2f, 0.35f * (1.0f - t) * std::min(1.0f, 4.0f * t)), glowCell);
            break;
        }
        case Kind::Arc: { // a wide faint glow under a thin bright core, flickering out
            const f32 alpha = t < 0.5f ? 1.0f : 0.55f;
            const Color glow = tone(0.4f, 0.6f, 1.0f, 0.55f * alpha);
            const Color core = tone(0.92f, 0.96f, 1.0f, alpha);
            for (u8 i = 0; i + 1 < piece.points; ++i) {
                const Vec3 from = body.feet + piece.path[i];
                const Vec3 to = body.feet + piece.path[i + 1];
                const Vec3 wide = acrossOf(to - from, frame.forward, frame.up, 0.5f * scale);
                strip(m_additive, from, to, wide, glow, glow, beamCell);
                const Vec3 thin = acrossOf(to - from, frame.forward, frame.up, piece.size);
                strip(m_additive, from, to, thin, core, core, beamCell);
            }
            if (piece.points > 0) { // sparks where it lands
                const f32 half = 0.3f * scale;
                quad(m_additive, body.feet + piece.path[0], frame.right * half, frame.up * half, core,
                     glintCell);
                quad(m_additive, body.feet + piece.path[piece.points - 1], frame.right * half,
                     frame.up * half, core, glintCell);
            }
            break;
        }
        case Kind::Static: {
            const f32 half = 0.5f * piece.size;
            quad(m_additive, at, frame.right * half, frame.up * half, tone(0.8f, 0.9f, 1.0f, 1.0f),
                 glintCell);
            break;
        }
        case Kind::Ring: { // flat on the ground, rolling outward and fading
            const f32 radius = glm::mix(piece.size, piece.phase, ease(t));
            quad(m_additive, at, Vec3{radius, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, radius},
                 tone(0.42f, 1.0f, 0.25f, 0.75f * (1.0f - t)), ringCell);
            break;
        }
        case Kind::Wisp: {
            const f32 half = 0.5f * piece.size * (1.0f + 0.8f * t);
            quad(m_additive, at, frame.right * half, frame.up * half,
                 toneMix(Vec3{0.5f, 1.0f, 0.35f}, Vec3{0.1f, 0.55f, 0.1f}, t,
                         0.7f * std::sin(kPi * t)),
                 glowCell);
            break;
        }
        case Kind::Drip: {
            quad(m_additive, at, frame.upright * (0.5f * piece.size), up * piece.size,
                 tone(0.6f, 1.0f, 0.3f, 0.95f), dropCell);
            break;
        }
        case Kind::Ray: { // out from within, to its full length, then gone
            const f32 length = piece.size * ease(piece.age / kRayRise);
            const f32 alpha = t < 0.3f ? ease(t / 0.3f) : ease((1.0f - t) / 0.7f);
            const Color colour =
                toneMix(Vec3{1.0f, 0.98f, 0.82f}, Vec3{1.0f, 0.85f, 0.45f}, t, alpha);
            const Vec3 to = at + piece.velocity * length;
            const Vec3 wide = acrossOf(piece.velocity, frame.forward, frame.right, 0.22f * scale);
            strip(m_additive, at, to, wide, colour, colour, streakCell);
            break;
        }
        case Kind::Glint: {
            const f32 swell = std::sin(kPi * t);
            const f32 half = 0.5f * piece.size * (0.6f + 0.8f * swell);
            quad(m_additive, at, frame.right * half, frame.up * half, tone(1.0f, 1.0f, 0.9f, swell),
                 glintCell);
            break;
        }
        default: { // a mote
            const f32 half = 0.5f * piece.size;
            quad(m_additive, at, frame.right * half, frame.up * half,
                 tone(1.0f, 0.95f, 0.7f, 0.8f * std::sin(kPi * t)), glowCell);
            break;
        }
        }
    }
}

void ElementAuraShow::clear() {
    m_auras.clear();
    m_additive.clear();
    m_alpha.clear();
}

void ElementAuraShow::release() {
    m_atlas.release();
}

usize ElementAuraShow::pieces() const {
    usize count = 0;
    for (const Aura& aura : m_auras) {
        count += aura.pieces.size();
    }
    return count;
}

usize ElementAuraShow::pieces(Kind kind) const {
    usize count = 0;
    for (const Aura& aura : m_auras) {
        count += static_cast<usize>(std::ranges::count_if(
            aura.pieces, [kind](const Piece& piece) { return piece.kind == kind; }));
    }
    return count;
}

std::vector<Vec3> ElementAuraShow::placesOf(u64 instance, Kind kind) const {
    std::vector<Vec3> places;
    for (const Aura& aura : m_auras) {
        if (aura.body.instance != instance) {
            continue;
        }
        for (const Piece& piece : aura.pieces) {
            if (piece.kind != kind) {
                continue;
            }
            if (kind == Kind::Arc) {
                for (u8 i = 0; i < piece.points; ++i) {
                    places.push_back(aura.body.feet + piece.path[i]);
                }
            } else {
                places.push_back(placeOf(aura.body, piece));
            }
        }
    }
    return places;
}

} // namespace gdl::game
