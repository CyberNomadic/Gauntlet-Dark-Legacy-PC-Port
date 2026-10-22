#include "game/level/LevelTransportersCheckpoint.h"

#include <algorithm>
#include <bit>
#include <exception>
#include <limits>
#include <set>
#include <utility>

#include "engine/assets/TextureBindings.h"
#include "engine/io/Sha256.h"

#include "game/level/ItemFigure.h"

namespace gdl::game {
namespace fc = fixture_checkpoint;
namespace {
s32 parameter(const ItemInstance& instance, usize offset) {
    u32 value = 0;
    for (usize byte = 0; byte < sizeof(value); ++byte) {
        value |= static_cast<u32>(instance.params[offset + byte]) << (byte * 8);
    }
    return std::bit_cast<s32>(value);
}
bool identity(const LevelTransportersCheckpoint::Pad& saved, const LevelTransporters::Pad& live) {
    return saved.source.index == static_cast<usize>(live.instance) && saved.id == live.id &&
           saved.destinationId == live.destinationId && saved.destination == live.destination &&
           saved.radius == live.radius && saved.height == live.height &&
           saved.support == live.support && saved.supportLocal == live.supportLocal &&
           saved.anchorLocal == live.anchorLocal && saved.model == live.model.bound() &&
           saved.pose.has_value() == live.pose.posed();
}

// Ordered, value-only resource identity. Paths, allocations and GPU handles never
// enter it. Only explicit binding visits native assets; restores use this catalog.
class Fingerprint {
public:
    void add(u64 value) {
        std::array<u8, 8> bytes{};
        for (u32 index = 0; index < 8; ++index) {
            bytes[index] = static_cast<u8>(value >> (index * 8));
        }
        m_hash.update(bytes);
    }
    void add(u32 value) { add(static_cast<u64>(value)); }
    void add(s32 value) { add(std::bit_cast<u32>(value)); }
    void add(u16 value) { add(static_cast<u32>(value)); }
    void add(u8 value) { add(static_cast<u32>(value)); }
    void add(bool value) { add(value ? 1U : 0U); }
    void add(f32 value) { add(std::bit_cast<u32>(value)); }
    void add(std::string_view value) {
        add(static_cast<u64>(value.size()));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): byte view of a trivially
        // copyable buffer
        m_hash.update({reinterpret_cast<const u8*>(value.data()), value.size()});
    }
    void add(const Vec2& value) {
        add(value.x);
        add(value.y);
    }
    void add(const Vec3& value) {
        add(value.x);
        add(value.y);
        add(value.z);
    }
    void add(const Color& value) {
        add(value.r);
        add(value.g);
        add(value.b);
        add(value.a);
    }
    template <class T> void sequence(const T& values) {
        add(static_cast<u64>(values.size()));
        for (const auto& value : values) {
            add(value);
        }
    }
    void tree(const TreeInfo& tree) {
        add(tree.name);
        add(tree.prefix);
        add(static_cast<u64>(tree.nodes.size()));
        for (const auto& node : tree.nodes) {
            add(node.name);
            add(node.object);
            add(node.type);
            add(node.flags);
            add(node.objectFlags);
            add(node.parent);
            add(node.position);
            add(node.particle);
            add(node.direction);
            add(node.textureAnimation);
            add(static_cast<u64>(node.objectFrames.size()));
            for (const auto& run : node.objectFrames) {
                add(run.object);
                add(run.start);
                add(run.frames);
            }
        }
        add(static_cast<u64>(tree.sequences.size()));
        for (const auto& sequence : tree.sequences) {
            add(sequence.name);
            add(sequence.frames);
            add(sequence.frameRate);
            add(sequence.repeats);
            add(sequence.fixesPosition);
            add(sequence.flags);
            add(sequence.textureAnimationStart);
            add(sequence.textureAnimationCount);
            add(static_cast<u64>(sequence.tracks.size()));
            for (const auto& track : sequence.tracks) {
                add(track.node);
                add(track.flags);
                this->sequence(track.frames);
                this->sequence(track.values);
            }
            this->sequence(sequence.trackOfNode);
        }
    }
    void animation(const TextureAnimationInfo& value) {
        add(value.name);
        add(value.frameName);
        add(value.texture);
        add(value.source);
        add(value.frames);
        add(value.start);
        add(value.rate);
        add(value.offset);
        add(value.flag);
    }
    void mesh(const Mesh& value) {
        add(value.prelit);
        add(value.flags);
        add(static_cast<u64>(value.vertices.size()));
        for (const auto& vertex : value.vertices) {
            add(vertex.position);
            add(vertex.normal);
            add(vertex.uv);
            add(vertex.lightmapUv);
            add(vertex.color);
        }
        add(static_cast<u64>(value.parts.size()));
        for (const auto& part : value.parts) {
            add(part.texture);
            add(part.lightmap);
            sequence(part.indices);
        }
    }
    std::array<u8, 32> finish() const { return m_hash.finish(); }
    void image(const Image& value) {
        add(value.width);
        add(value.height);
        add(static_cast<u64>(value.pixels.size()));
        m_hash.update(value.pixels);
    }

private:
    Sha256 m_hash;
};

std::optional<std::array<u8, 32>> catalog(ItemArchive* archive, const TreeInfo* tree) {
    Fingerprint hash;
    hash.add(std::string_view{"LevelTransporters/1"});
    hash.add(tree != nullptr);
    if (!tree) {
        return hash.finish();
    }
    if (!archive || !archive->loaded()) {
        return std::nullopt;
    }
    hash.tree(*tree);
    std::set<u32> models;
    std::set<u32> images;
    const auto addModels = [&](const std::string& name, s32 count) {
        const auto first = archive->models.find(name);
        hash.add(first.has_value());
        if (first) {
            hash.add(*first);
            for (s32 frame = 0;
                 frame < count && *first + static_cast<u32>(frame) < archive->models.size();
                 ++frame) {
                models.insert(*first + static_cast<u32>(frame));
            }
        }
    };
    for (const auto& node : tree->nodes) {
        if (node.name == "DUMMY" || node.name == "NULL1") {
            continue;
        }
        if (!node.object.empty()) {
            addModels(node.object, 1);
        }
        for (const auto& run : node.objectFrames) {
            if (!run.object.empty()) {
                addModels(run.object, run.frames);
            }
        }
    }
    hash.add(static_cast<u64>(models.size()));
    for (const auto index : models) {
        const auto& entry = archive->models.entry(index);
        const auto& mesh = archive->models.mesh(index);
        hash.add(index);
        hash.add(entry.name);
        hash.add(entry.triangles);
        hash.add(entry.flags);
        hash.mesh(mesh);
        for (const auto& part : mesh.parts) {
            const auto binding = TextureBindings(archive->textures).slot(part.texture);
            hash.add(binding.has_value());
            if (binding) {
                images.insert(binding->index);
            }
        }
    }
    const auto& animations = archive->trees.textureAnimations();
    hash.add(static_cast<u64>(animations.size()));
    for (const auto& animation : animations) {
        hash.animation(animation);
        if (!animation.cycles() || animation.texture < 0 || animation.frames == 0) {
            continue;
        }
        const auto first = animation.source >= 0
                               ? std::optional<TextureBinding>{{&archive->textures,
                                                                static_cast<u32>(animation.source)}}
                               : TextureBindings(archive->textures).image(animation.frameName);
        hash.add(first.has_value());
        if (!first) {
            continue;
        }
        hash.add(first->index);
        const s64 count = std::abs(static_cast<s64>(animation.frames));
        for (u64 frame = 0;
             frame < static_cast<u64>(count) && first->index + frame < archive->textures.size();
             ++frame) {
            images.insert(first->index + static_cast<u32>(frame));
        }
    }
    // All slots participate in authored index/name resolution; only selected
    // images are decoded (the same no-lender bank the native owner binds).
    hash.add(static_cast<u64>(archive->textures.size()));
    for (u32 index = 0; index < archive->textures.size(); ++index) {
        const auto& entry = archive->textures.entry(index);
        hash.add(entry.name);
        hash.add(entry.width);
        hash.add(entry.height);
        hash.add(entry.flags);
        hash.add(entry.frames);
        hash.add(entry.halfResolution);
        hash.add(entry.clampU);
        hash.add(entry.clampV);
        hash.add(entry.noPicture);
    }
    hash.add(static_cast<u64>(images.size()));
    for (const u32 index : images) {
        hash.add(index);
        hash.image(archive->textures.image(index));
    }
    return hash.finish();
}
} // namespace

bool LevelTransportersCheckpoint::State::valid() const {
    if (pads.size() > kMaxPads || players < 1 || players > 4 ||
        (tree != "TRANS" && !tree.empty()) || !textures.valid() ||
        (!presentationAdvanced && textures.advance != 0)) {
        return false;
    }
    usize details = 0;
    for (usize index = 0; index < pads.size(); ++index) {
        const auto& pad = pads[index];
        const auto& source = pad.source;
        if (!source.valid() || source.info.type != LevelTransporters::kItemType ||
            (index != 0 && source.index <= pads[index - 1].source.index) ||
            !shownToParty(source.instance.minPlayers, players) ||
            pad.id != parameter(source.instance, 0) ||
            pad.destinationId != parameter(source.instance, 4) ||
            pad.radius != source.info.radius || pad.height != source.info.height ||
            pad.radius < 0 || pad.height < 0 || !fc::vector(pad.position) ||
            !fc::matrix(pad.transform) || !fc::matrix(pad.supportLocal) ||
            !fc::vector(pad.anchorLocal) || pad.support < -1 || pad.support > 1'000'000 ||
            (pad.support == -1 && (pad.supportLocal != Mat4{1} || pad.anchorLocal != Vec3{0})) ||
            pad.model != pad.pose.has_value() || (pad.model && tree.empty()) ||
            (pad.pose && (!pad.pose->valid() || pad.pose->tree != tree)) ||
            !pad.animation.valid() || !fc::scalar(pad.previousFrame) || pad.previousFrame < 0 ||
            pad.previousGeneration > pad.animation.generation ||
            (!pad.model && (pad.animation != AnimationPlayer::Checkpoint{} ||
                            pad.previousFrame != 0 || pad.previousGeneration != 0))) {
            return false;
        }
        std::optional<usize> destination;
        for (usize other = 0; other < pads.size(); ++other) {
            if (index != other && pads[other].id == pad.destinationId) {
                destination = other;
                break;
            }
        }
        if (pad.destination != destination || source.info.choices.size() > kMaxDetails - details) {
            return false;
        }
        details += source.info.choices.size();
        if (source.instance.collision.size() > kMaxDetails - details) {
            return false;
        }
        details += source.instance.collision.size();
    }
    return true;
}

struct LevelTransportersCheckpoint::Resources {
    struct Row {
        Pad initial;
        const LevelTransporters::Pad* address = nullptr;
    };
    std::vector<Row> rows;
    std::array<u8, 32> catalog{};
    const WorldLayout* layout = nullptr;
    ItemArchive* archive = nullptr;
    const TreeInfo* tree = nullptr;
    const WorldCollision* collision = nullptr;
    s32 players = 1;
    std::shared_ptr<const u8> binding;
};
struct LevelTransportersCheckpoint::Prepared::Data {
    const LevelTransportersCheckpoint* wrapper = nullptr;
    std::shared_ptr<const Resources> resources;
    std::shared_ptr<const u8> lifetime;
    u64 generation = 0;
    std::vector<LevelTransporters::Pad> pads;
    TextureAnimator textures;
    bool presentationAdvanced = false;
};
LevelTransportersCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data)
    : m_data(std::move(data)) {}
LevelTransportersCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
LevelTransportersCheckpoint::Prepared&
LevelTransportersCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
LevelTransportersCheckpoint::Prepared::~Prepared() = default;
LevelTransportersCheckpoint::LevelTransportersCheckpoint(LevelTransporters& owner)
    : m_owner(owner) {}
LevelTransportersCheckpoint::~LevelTransportersCheckpoint() = default;

bool LevelTransportersCheckpoint::bound() const {
    if (!m_resources || !m_owner.m_layout || m_resources->binding != m_owner.m_checkpointBinding ||
        m_resources->layout != m_owner.m_layout || m_resources->archive != m_owner.m_archive ||
        m_resources->tree != m_owner.m_tree || m_resources->collision != m_owner.m_collision ||
        m_resources->players != m_owner.m_players || m_resources->rows.size() != m_owner.size() ||
        (m_resources->archive && !m_resources->archive->loaded())) {
        return false;
    }
    for (usize index = 0; index < m_resources->rows.size(); ++index) {
        const auto& row = m_resources->rows[index];
        if (row.address != &m_owner.m_pads[index] ||
            !row.initial.source.matches(*m_owner.m_layout) ||
            !identity(row.initial, m_owner.m_pads[index])) {
            return false;
        }
    }
    return true;
}
bool LevelTransportersCheckpoint::bindResources() {
    m_resources.reset();
    if (!m_owner.m_layout || m_owner.size() > State::kMaxPads) {
        return false;
    }
    auto resources = std::make_shared<Resources>();
    resources->binding = m_owner.m_checkpointBinding;
    resources->layout = m_owner.m_layout;
    resources->archive = m_owner.m_archive;
    resources->tree = m_owner.m_tree;
    resources->collision = m_owner.m_collision;
    resources->players = m_owner.m_players;
    try {
        const auto fingerprint = catalog(resources->archive, resources->tree);
        if (!fingerprint) {
            return false;
        }
        resources->catalog = *fingerprint;
    } catch (const std::exception&) {
        return false;
    }
    for (const auto& live : m_owner.m_pads) {
        if (live.instance < 0) {
            return false;
        }
        const auto source = fc::placement(resources->layout, static_cast<usize>(live.instance));
        const auto pose = live.pose.checkpoint();
        if (!source || (live.pose.posed() && !pose)) {
            return false;
        }
        resources->rows.push_back(
            {{*source, live.id, live.destinationId, live.destination, live.position, live.radius,
              live.height, live.transform, live.support, live.supportLocal, live.anchorLocal,
              live.model.bound(), pose, live.animation.checkpoint(), live.previousFrame,
              live.previousGeneration},
             &live});
    }
    m_resources = std::move(resources);
    m_lifetime = std::make_shared<const u8>(0);
    if (!checkpoint()) {
        m_resources.reset();
        return false;
    }
    return true;
}
bool LevelTransportersCheckpoint::binds(const LevelTransporters& owner) const {
    return &owner == &m_owner && bound();
}
bool LevelTransportersCheckpoint::binds(const WorldLayout& layout,
                                        const WorldCollision* collision) const {
    return m_owner.m_layout == &layout && m_owner.m_collision == collision && bound();
}
std::optional<LevelTransportersCheckpoint::State> LevelTransportersCheckpoint::checkpoint() const {
    if (!bound()) {
        return std::nullopt;
    }
    State state;
    state.catalog = m_resources->catalog;
    state.tree = m_owner.m_tree ? m_owner.m_tree->name : "";
    state.players = m_owner.m_players;
    state.collision = m_owner.m_collision != nullptr;
    state.textures = m_owner.m_textures.checkpoint();
    state.presentationAdvanced = m_owner.m_presentationAdvanced;
    state.pads.reserve(m_owner.size());
    for (usize index = 0; index < m_owner.size(); ++index) {
        const auto& live = m_owner.m_pads[index];
        const auto pose = live.pose.checkpoint();
        if (live.pose.posed() && !pose) {
            return std::nullopt;
        }
        state.pads.push_back({m_resources->rows[index].initial.source, live.id, live.destinationId,
                              live.destination, live.position, live.radius, live.height,
                              live.transform, live.support, live.supportLocal, live.anchorLocal,
                              live.model.bound(), pose, live.animation.checkpoint(),
                              live.previousFrame, live.previousGeneration});
    }
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}
std::optional<LevelTransportersCheckpoint::Prepared>
LevelTransportersCheckpoint::prepareRestore(const State& state) const {
    if (!bound() || !state.valid() || state.catalog != m_resources->catalog ||
        state.players != m_owner.m_players || state.collision != (m_owner.m_collision != nullptr) ||
        state.tree != (m_owner.m_tree ? m_owner.m_tree->name : "") ||
        state.pads.size() != m_owner.size() ||
        m_owner.m_checkpointGeneration == std::numeric_limits<u64>::max()) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->wrapper = this;
    data->resources = m_resources;
    data->lifetime = m_lifetime;
    data->generation = m_owner.m_checkpointGeneration;
    data->textures = m_owner.m_textures;
    data->presentationAdvanced = state.presentationAdvanced;
    if (!data->textures.restore(state.textures)) {
        return std::nullopt;
    }
    data->pads.reserve(state.pads.size());
    for (usize index = 0; index < state.pads.size(); ++index) {
        const auto& saved = state.pads[index];
        const auto& live = m_owner.m_pads[index];
        if (saved.source != m_resources->rows[index].initial.source || !identity(saved, live)) {
            return std::nullopt;
        }
        LevelTransporters::Pad pad = live; // CPU values; immutable meshes/textures remain borrowed.
        const TreeSequenceInfo* sequence = nullptr;
        if (saved.animation.playing) {
            const auto active =
                m_owner.m_tree ? m_owner.m_tree->findSequence("ACTIVE") : std::nullopt;
            if (!active || saved.animation.sequence != *active) {
                return std::nullopt;
            }
            // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): the pointer is bound whenever the
            // index it guards is set
            sequence = &m_owner.m_tree->sequences[*active];
            const f32 period = (sequence->frameRate > 0 ? static_cast<f32>(sequence->frameRate)
                                                        : AnimationPlayer::kDefaultRate) *
                               AnimationPlayer::kRateUnit;
            if (saved.previousFrame > static_cast<f32>(std::max(sequence->frames, 0)) ||
                saved.animation.secondsPerFrame != period || saved.animation.speed != 1 ||
                saved.animation.smooth || saved.animation.held ||
                saved.animation.transitionLength != 0) {
                return std::nullopt;
            }
        } else if (saved.animation != AnimationPlayer::Checkpoint{} || saved.previousFrame != 0 ||
                   saved.previousGeneration != 0 || live.animation.playing()) {
            return std::nullopt;
        }
        if (!pad.animation.restore(saved.animation, sequence) ||
            (saved.pose && (!m_owner.m_tree || !pad.pose.restore(*m_owner.m_tree, *saved.pose)))) {
            return std::nullopt;
        }
        pad.position = saved.position;
        pad.transform = saved.transform; // Deliberately do not sync the current support here.
        pad.previousFrame = saved.previousFrame;
        pad.previousGeneration = saved.previousGeneration;
        if (m_owner.m_tree && pad.animation.playing()) {
            pad.model.setFrame(pad.animation.sequence(), static_cast<s32>(pad.animation.frame()));
            data->textures.apply(pad.model, *m_owner.m_tree, pad.animation.sequence(),
                                 static_cast<s32>(pad.animation.frame()));
        }
        data->pads.push_back(std::move(pad));
    }
    Prepared prepared{std::move(data)};
    return canCommit(prepared) ? std::optional{std::move(prepared)} : std::nullopt;
}
bool LevelTransportersCheckpoint::canCommit(const Prepared& prepared) const {
    return prepared.m_data && prepared.m_data->wrapper == this &&
           prepared.m_data->resources == m_resources && prepared.m_data->lifetime == m_lifetime &&
           prepared.m_data->generation == m_owner.m_checkpointGeneration && bound();
}
bool LevelTransportersCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    for (usize index = 0; index < data->pads.size(); ++index) {
        m_owner.m_pads[index] = std::move(data->pads[index]);
    }
    m_owner.m_textures = std::move(data->textures);
    m_owner.m_presentationAdvanced = data->presentationAdvanced;
    ++m_owner.m_checkpointGeneration;
    return true;
}
bool LevelTransportersCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl::game
