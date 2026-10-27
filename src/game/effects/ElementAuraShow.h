#pragma once

#include <array>
#include <span>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCamera.h"
#include "game/effects/ElementAura.h"

namespace gdl::game {

/**
 * The show on Aggressor Mode's elemental ones, presentation only and never simulation state.
 * Fire burns off the body the way the fire potion's does: the weapons archive's own flame
 * flipbook licks up off the skin over soft breath puffs and embers (the atlas's flames stand
 * in without the archive); lightning crawls over it in arcs while static crackles on its
 * skin; acid wraps it in a green haze that rolls out over the ground in rings and drips off
 * it; holy light breaks out of it in rays and glints. The bodies are told every frame, and
 * an aura lives while its body does, following it.
 */
class ElementAuraShow {
public:
    /** An elemental body, by the lifetime its pool slot holds. */
    struct Body {
        u64 instance = 0;
        s32 element = 0; ///< 1 fire, 2 electric, 3 holy, 4 acid
        Vec3 feet{0.0f, 0.0f, 0.0f};
        f32 radius = 1.5f;
        f32 height = 6.0f;
    };
    enum class Kind : u8 { Tongue, Ember, Puff, Arc, Static, Ring, Wisp, Drip, Ray, Glint, Mote };
    static constexpr usize kMostPieces = 96; ///< one body's pieces at once
    static constexpr usize kMostAuras = 64;
    static constexpr f32 kBodyRadius = 1.5f; ///< the swarm's usual; the show scales from it
    static constexpr f32 kRingLift = 0.25f;  ///< the ground rings' height over the feet
    /** The fire potion's flame flipbook in the weapons archive: ten frames, one a game
     * frame, a flame growing and dying; and the soft puff its burst breathes. */
    static constexpr std::string_view kFlameFrame = "FIRE_POTION00";
    static constexpr u32 kFlameFrames = 10;
    static constexpr f32 kFlameRate = 30.0f;
    static constexpr std::string_view kPuffSprite = "DRAGONBREATH";

    /** Keeps an aura on each body, `seconds` on from the last frame. */
    void step(std::span<const Body> bodies, f32 seconds);
    /** Draws every aura, additive and writing no depth: over the atlas, and over the fire
     * potion's own sprites when `weapons` is loaded and holds them. */
    void draw(RenderDevice& device, const Mat4& clip, const CameraFrame& camera,
              ItemArchive* weapons = nullptr);
    void clear();
    void release();

    usize auras() const { return m_auras.size(); }
    usize pieces() const;
    usize pieces(Kind kind) const;
    /** Where the pieces of `kind` on the aura of `instance` are, every point of an arc. */
    std::vector<Vec3> placesOf(u64 instance, Kind kind) const;
    /** Whether the last draw had the fire potion's sprites to burn with. */
    bool retailFire() const { return m_retail.puff != nullptr; }

private:
    struct Piece {
        Kind kind = Kind::Ember;
        f32 age = 0.0f;
        f32 life = 1.0f;
        Vec3 origin{0.0f, 0.0f, 0.0f};   ///< from the feet; a ray's start, an arc's unused
        Vec3 velocity{0.0f, 0.0f, 0.0f}; ///< a ray's direction
        f32 size = 1.0f;                 ///< a ray's full length, a ring's first radius
        f32 phase = 0.0f;                ///< a ring's last radius
        u8 frame = 0;
        u8 points = 0;
        std::array<Vec3, 8> path{}; ///< an arc's points, from the feet
    };
    struct Aura {
        Body body;
        f32 age = 0.0f;
        std::array<f32, 3> owed{}; ///< births owed, a lane a kind
        f32 nextBurst = 0.0f;
        u32 random = 1;
        bool seen = false;
        std::vector<Piece> pieces;
    };
    struct Frame {
        Vec3 right{1.0f, 0.0f, 0.0f};
        Vec3 up{0.0f, 1.0f, 0.0f};
        Vec3 forward{0.0f, 0.0f, 1.0f};
        Vec3 upright{1.0f, 0.0f, 0.0f}; ///< the right of a sprite that stands up
    };
    /** The fire potion's sprites, borrowed from the archive last drawn with. */
    struct RetailFire {
        ItemArchive* archive = nullptr;
        bool tried = false;
        std::array<const Texture*, kFlameFrames> flames{};
        const Texture* puff = nullptr;
    };

    static void advance(Aura& aura, f32 seconds);
    static void owe(Aura& aura, usize lane, f32 perSecond, Kind kind, f32 seconds);
    static void bear(Aura& aura, Kind kind);
    static void bearArc(Aura& aura);
    static void bearRing(Aura& aura, const Vec3& at, f32 from, f32 to, f32 life);
    void bindRetail(RenderDevice& device, ItemArchive* weapons);
    void drawAura(const Aura& aura, const Frame& frame, bool retail);
    static Vec3 placeOf(const Body& body, const Piece& piece);

    std::vector<Aura> m_auras;
    ElementAura m_atlas;
    RetailFire m_retail;
    ImmediateBatch m_additive;                         ///< over the atlas
    std::array<ImmediateBatch, kFlameFrames> m_flames; ///< over each flipbook frame
    ImmediateBatch m_puffs;                            ///< over the breath puff
};

} // namespace gdl::game
