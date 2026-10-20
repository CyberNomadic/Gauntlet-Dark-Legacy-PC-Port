#pragma once

#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "engine/io/NativeStateDigest.h"

namespace gdl {
struct ItemArchive;
class AnimationSet;
struct TreeInfo;
class Texture;
class TextureSet;
class TextureAnimator;
class TreeModel;
class WorldCollision;
class WorldLayout;
struct Image;
}

namespace gdl::game {

class CritterData;
struct CombatantAssets;
class HazardSurfaces;
struct LookoutRoute;

/** Same-loaded-stage resource identities for native state hashing. Registration
 * observes existing bindings only; lookup never loads assets or touches a device.
 * Keys are logical asset/role identifiers shared by peers, never absolute paths,
 * allocation addresses, speculative birth counters or load-order indices.
 *
 * The caller authenticates the build and asset data separately, registers every
 * archive before its borrowers, and clears this registry before releasing or
 * rebinding any resource. Retained speculative bindings may stay registered.
 * Generated construction is fingerprinted at registration; registered resources
 * must remain immutable. Mutable clocks/poses belong in their native visitors.
 * A returned Resources view borrows this registry and cannot outlive it. */
class NativeStateResources {
public:
    NativeStateResources() = default;
    NativeStateResources(const NativeStateResources&) = delete;
    NativeStateResources& operator=(const NativeStateResources&) = delete;
    NativeStateResources(NativeStateResources&&) = delete;
    NativeStateResources& operator=(NativeStateResources&&) = delete;
    ~NativeStateResources() = default;

    /** Index authored trees/sequences once, then register any already-created
     * textures. Repeating a binding is harmless and admits newly lazy-loaded art. */
    bool registerArchive(const ItemArchive& archive, std::string_view logicalKey);
    bool refreshArchive(const ItemArchive& archive);
    /** Standalone character action banks, without a synthetic ItemArchive. */
    bool registerAnimations(const AnimationSet& animations, std::string_view logicalKey);
    /** Standalone world texture banks use the same slot identity as archive
     * banks. Give one bank the same logical key on every registration. An empty
     * optional lender still has a semantic identity; loading/rebinding it later
     * requires the same registry/history invalidation as every other asset. */
    bool registerTextures(const TextureSet& textures, std::string_view logicalKey);
    /** Observe newly cached textures in an already identified lender bank. */
    bool refreshTextures(const TextureSet& textures);
    bool registerTexture(const Texture& texture, const ItemArchive& archive, u32 slot);
    /** Renderer-owned immutable fallback, such as whiteTexture() registered as
     * "opaque-white". The caller supplies its established semantic role; the
     * registry never inspects GPU contents or creates the texture. */
    bool registerBuiltinTexture(const Texture& texture, std::string_view role);
    /** Immutable standalone pixels, fingerprinted once. Authored images are
     * registered by bank/slot through registerTextures without decoding. */
    bool registerImage(const Image& image, std::string_view role);
    /** Code-created immutable animation without an archive, such as IT's hidden
     * READY tree. The role and complete construction distinguish the binding. */
    bool registerBuiltinTree(const TreeInfo& tree, std::string_view role);
    /** The role names the actual bound model (for example enemy kind/shot slot),
     * not a guessed texture/tree name. Include any construction variation in it. */
    bool registerModel(const TreeModel& model, const ItemArchive& archive,
                       std::string_view bindingKey);
    bool registerStaticTree(const TreeInfo& tree, const ItemArchive& archive, u32 modelIndex,
                            u32 objectFlags);
    bool registerGeneratedTree(const TreeInfo& tree, const ItemArchive& archive,
                               std::string_view bindingKey);
    bool registerTextureAnimator(const TextureAnimator& animator, const ItemArchive& archive,
                                 std::string_view bindingKey);
    bool registerCritter(const CritterData& data, std::string_view wadKey, u32 typeIndex);
    /** Registers the retained family, including child definitions and keyed skins.
     * Does not activate a family removed from the logical roster by a rewind. */
    bool registerCombatantAssets(const CombatantAssets& assets, std::string_view logicalKey);
    /** Identifies the bound stage, not its mutable collision state; that state
     * must be hashed separately by its actual owner. */
    bool registerCollision(const WorldCollision& collision, std::string_view stageKey);
    /** Immutable authored layout identity; mutable transforms belong to its world. */
    bool registerLayout(const WorldLayout& layout, std::string_view stageKey);
    bool registerHazards(const HazardSurfaces& hazards, std::string_view stageKey);
    /** Identity of a mutable route owner, not its current contents. The owner's
     * saved value is visited separately, so saved patrols never read a live route. */
    bool registerLookouts(const LookoutRoute& route, std::string_view ownerKey);

    NativeStateDigest::Resources resources() const;
    bool good() const { return m_good; }
    void clear();

private:
    using Identity = NativeStateDigest::Digest;
    struct Key {
        std::type_index type;
        const void* address;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        usize operator()(const Key& key) const;
    };
    struct ArchiveBinding {
        std::string key;
        Identity identity;
    };
    struct TextureBinding {
        std::string key;
        Identity identity;
        std::vector<const Texture*> slots;
        std::vector<const Image*> images;
    };
    std::unordered_map<Key, Identity, KeyHash> m_bindings;
    std::unordered_map<const ItemArchive*, ArchiveBinding> m_archives;
    std::unordered_map<const AnimationSet*, ArchiveBinding> m_animationSets;
    std::unordered_map<const TextureSet*, TextureBinding> m_textureSets;
    std::unordered_map<const Image*, ArchiveBinding> m_images;
    bool m_good = true;

    bool bind(std::type_index type, const void* address, const Identity& value);
    const ArchiveBinding* archiveBinding(const ItemArchive& archive);
    bool bindTree(const TreeInfo& tree, const Identity& value);
    bool reject();
    static bool append(NativeStateDigest& digest, std::type_index type, const void* address,
                       void* context);
};

} // namespace gdl::game
