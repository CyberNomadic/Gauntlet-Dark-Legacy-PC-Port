#include "game/netplay/MatchSession.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "engine/core/Log.h"
#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDLT");
void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
bool validOwners(const MatchOwners& owners) {
    return std::ranges::all_of(owners, [](u8 peer) { return peer <= 4; }) &&
           std::ranges::find(owners, 1) != owners.end();
}
bool belongs(const MatchOwners& owners, u8 peer) {
    return peer != 0 && std::ranges::find(owners, peer) != owners.end();
}
u8 seatsOf(const MatchOwners& owners, u8 peer = 0) {
    u8 mask = 0;
    for (usize seat = 0; seat < owners.size(); ++seat) {
        if (owners[seat] != 0 && (peer == 0 || owners[seat] == peer)) {
            mask |= static_cast<u8>(1U << seat);
        }
    }
    return mask;
}
bool checkpointed(MatchTransition transition) {
    return transition == MatchTransition::Travel || transition == MatchTransition::Shop ||
           transition == MatchTransition::SecretEnter ||
           transition == MatchTransition::SecretReturn;
}
bool hasEntryMovie(MatchTransition transition) {
    return transition == MatchTransition::Start || transition == MatchTransition::Travel;
}
std::string_view agreementReason(NativeAgreement::Failure reason) {
    switch (reason) {
    case NativeAgreement::Failure::None: return "none";
    case NativeAgreement::Failure::Mismatch: return "mismatch";
    case NativeAgreement::Failure::Conflict: return "conflicting digest";
    case NativeAgreement::Failure::Timeout: return "timeout";
    case NativeAgreement::Failure::Window: return "bounded window exhausted";
    case NativeAgreement::Failure::Protocol: return "invalid agreement protocol";
    case NativeAgreement::Failure::Disconnected: return "disconnected";
    case NativeAgreement::Failure::Transport: return "transport failure";
    }
    return "unknown";
}
} // namespace

bool MatchContext::valid() const {
    if (epoch == 0 || scene == 0 || scene > 65535 || tickRate != 60 ||
        inputLead > LockstepInputs::kMaxDelay || transition == MatchTransition::Resume ||
        static_cast<u8>(transition) > 5 || !validOwners(owners) ||
        (challengeWon && transition != MatchTransition::SecretReturn)) {
        return false;
    }
    const bool fenced = checkpointed(transition);
    if (fenced ? (sourceEpoch == 0 || sourceEpoch != epoch - 1)
               : (sourceEpoch != 0 || sourceTick != 0)) {
        return false;
    }
    for (usize seat = 0; seat < owners.size(); ++seat) {
        if ((owners[seat] != 0) != (grants[seat] != 0)) {
            return false;
        }
    }
    return true;
}
bool MatchControl::valid() const {
    const bool vote = kind == MatchControlKind::MovieSkipVote;
    return context.valid() && static_cast<u8>(kind) <= 6 && static_cast<u8>(stop) <= 4 &&
           ((kind == MatchControlKind::Stop) == (stop != MatchStop::None)) &&
           (vote == (movieSkipVotes != 0)) && (movieSkipVotes & ~seatsOf(context.owners)) == 0 &&
           (!vote || hasEntryMovie(context.transition)) && kind != MatchControlKind::Pause &&
           kind != MatchControlKind::RequestPause;
}
std::optional<std::vector<u8>> MatchControlPacket::encode(const MatchControl& control) {
    if (!control.valid()) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(kBytes);
    word(bytes, kMagic);
    bytes.insert(bytes.end(), {kVersion, static_cast<u8>(control.kind),
                               static_cast<u8>(control.stop), control.movieSkipVotes});
    const auto& context = control.context;
    word(bytes, static_cast<u32>(context.epoch));
    word(bytes, static_cast<u32>(context.epoch >> 32U));
    word(bytes, context.scene);
    bytes.insert(bytes.end(),
                 {static_cast<u8>(context.tickRate), static_cast<u8>(context.tickRate >> 8U),
                  context.inputLead, static_cast<u8>(context.transition)});
    bytes.insert(bytes.end(), context.owners.begin(), context.owners.end());
    for (const auto grant : context.grants) {
        word(bytes, grant);
    }
    word(bytes, static_cast<u32>(context.challengeWon));
    word(bytes, static_cast<u32>(context.sourceEpoch));
    word(bytes, static_cast<u32>(context.sourceEpoch >> 32U));
    word(bytes, static_cast<u32>(context.sourceTick));
    word(bytes, static_cast<u32>(context.sourceTick >> 32U));
    return bytes;
}
std::optional<MatchControl> MatchControlPacket::decode(std::span<const u8> bytes) {
    if (bytes.size() != kBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    if (reader.readU32() != kMagic || reader.readU8() != kVersion) {
        return std::nullopt;
    }
    MatchControl control;
    control.kind = static_cast<MatchControlKind>(reader.readU8());
    control.stop = static_cast<MatchStop>(reader.readU8());
    control.movieSkipVotes = reader.readU8();
    auto& context = control.context;
    context.epoch = reader.readU32();
    context.epoch |= static_cast<u64>(reader.readU32()) << 32U;
    context.scene = reader.readU32();
    context.tickRate = reader.readU16();
    context.inputLead = reader.readU8();
    context.transition = static_cast<MatchTransition>(reader.readU8());
    for (auto& owner : context.owners) {
        owner = reader.readU8();
    }
    for (auto& grant : context.grants) {
        grant = reader.readU32();
    }
    const auto challengeWon = reader.readU32();
    if (challengeWon > 1) {
        return std::nullopt;
    }
    context.challengeWon = challengeWon != 0;
    context.sourceEpoch = reader.readU32();
    context.sourceEpoch |= static_cast<u64>(reader.readU32()) << 32U;
    context.sourceTick = reader.readU32();
    context.sourceTick |= static_cast<u64>(reader.readU32()) << 32U;
    return control.valid() ? std::optional{control} : std::nullopt;
}

bool MatchSession::open(u8 localPeer, const MatchOwners& owners, std::span<const MatchLink> links,
                        u8 startupDelay) {
    if (m_phase != Phase::Offline || !validOwners(owners) || !belongs(owners, localPeer) ||
        startupDelay > LockstepInputs::kMaxDelay) {
        return false;
    }
    std::array<PacketTransport::Connection, 5> connections{};
    for (const auto& link : links) {
        if (!belongs(owners, link.peer) || link.peer == localPeer || link.connection == 0 ||
            (localPeer != 1 && link.peer != 1) || connections[link.peer] != 0 ||
            std::ranges::find(connections, link.connection) != connections.end()) {
            return false;
        }
        connections[link.peer] = link.connection;
    }
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (peer != localPeer && belongs(owners, peer) && (localPeer == 1 || peer == 1) &&
            connections[peer] == 0) {
            return false;
        }
    }
    m_local = localPeer;
    m_owners = owners;
    m_links = connections;
    m_startupDelay = startupDelay;
    m_phase = Phase::Lobby;
    return true;
}
void MatchSession::clear() {
    *this = MatchSession{};
}
u8 MatchSession::peerOf(PacketTransport::Connection connection) const {
    if (connection != 0) {
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (m_links[peer] == connection) {
                return peer;
            }
        }
    }
    return 0;
}
bool MatchSession::queue(u8 peer, MatchControlKind kind, MatchStop reason, u8 votes) {
    const auto bytes = MatchControlPacket::encode({kind, reason, m_context, votes});
    if (peer == 0 || peer > 4 || m_links[peer] == 0 || !bytes ||
        m_controls.size() >= kControlBudget) {
        return false;
    }
    m_controls.push_back({m_links[peer], *bytes});
    return true;
}
bool MatchSession::broadcast(MatchControlKind kind, MatchStop reason, u8 votes) {
    const auto peers =
        std::ranges::count_if(m_links, [](auto connection) { return connection != 0; });
    if (m_controls.size() + static_cast<usize>(peers) > kControlBudget) {
        return false;
    }
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (m_links[peer] != 0 && !queue(peer, kind, reason, votes)) {
            return false;
        }
    }
    return true;
}
void MatchSession::discardGameplay() {
    m_lockstep.reset();
    m_nativeAgreement.reset();
    m_nativeResolved = {};
    m_nativeCompleted = 0;
    m_nativeInFlight.reset();
    m_pendingNativeContext.reset();
    m_pendingNativeParty = {};
    m_pendingNativeShop.reset();
    m_pendingNativeMovieVotes = 0;
    m_drainingInputs.reset();
    m_drainingContext.reset();
    m_sampled.clear();
}
bool MatchSession::beginNativeInputs() {
    if (m_context.transition == MatchTransition::Shop) {
        return true;
    }
    auto inputs = std::make_unique<LockstepInputs>();
    std::vector<LockstepInputs::Link> links;
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (m_links[peer] != 0) {
            links.push_back({peer, m_links[peer]});
        }
    }
    if (!inputs->open(m_local, m_context.epoch, m_owners, m_context.grants, m_context.inputLead,
                      links)) {
        return false;
    }
    m_lockstep = std::move(inputs);
    return true;
}
bool MatchSession::ensureNativeAgreement(Time now) {
    if (m_nativeAgreement) {
        return true;
    }
    auto agreement = std::make_unique<NativeAgreement>();
    std::vector<NativeAgreement::Link> links;
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (m_links[peer] != 0) {
            links.push_back({peer, m_links[peer]});
        }
    }
    if (!agreement->open(m_local, m_context.epoch, m_owners, links, kNativeAgreementPeriod, now)) {
        failNativeAgreement(NativeAgreement::Failure::Protocol);
        return false;
    }
    m_nativeAgreement = std::move(agreement);
    return true;
}
void MatchSession::failNativeAgreement(NativeAgreement::Failure fallback, u64 tick) {
    const auto reason =
        m_nativeAgreement && m_nativeAgreement->failed() ? m_nativeAgreement->failure() : fallback;
    if (m_nativeAgreement && m_nativeAgreement->failed()) {
        tick = m_nativeAgreement->failureTick();
    }
    log::error("Native observation agreement failed: {} (epoch {}, tick {}, peer {})",
               agreementReason(reason), m_context.epoch, tick, m_local);
    // The existing shared Stop supersedes queued observation packets and is
    // reliably retained even when a normal output queue is congested.
    stop(MatchStop::TransportFailure, true);
}
bool MatchSession::acceptPreparedContext(const MatchContext& context, MatchParty party) {
    m_context = context;
    m_travelParty = std::move(party);
    m_shop.begin(m_context.epoch, m_local, m_owners, m_context.grants);
    discardGameplay();
    if (!beginNativeInputs()) {
        stop(MatchStop::TransportFailure, false);
        return false;
    }
    m_loaded = {};
    m_movieSkipVotes = 0;
    m_movieSkipRequests = 0;
    m_loadingSeconds = 0;
    m_phase = Phase::Loading;
    return true;
}
bool MatchSession::finishNativeDrain() {
    if (!m_pendingNativeContext || m_nativeInFlight ||
        m_nativeCompleted != m_pendingNativeContext->sourceTick) {
        return false;
    }
    const auto context = *m_pendingNativeContext;
    auto party = std::move(m_pendingNativeParty);
    auto shop = std::move(m_pendingNativeShop);
    const auto votes = m_pendingNativeMovieVotes;
    if (!acceptPreparedContext(context, std::move(party))) {
        return false;
    }
    if (shop) {
        m_shop = std::move(*shop);
    }
    m_movieSkipVotes = votes;
    return true;
}
bool MatchSession::prepare(u32 scene, MatchTransition transition, const MatchParty* party,
                           const ShopSetup* shop, bool challengeWon) {
    const bool withCheckpoint = checkpointed(transition);
    const bool allowed = (transition == MatchTransition::Start && m_phase == Phase::Lobby) ||
                         (withCheckpoint && m_phase == Phase::Running);
    if (!host() || !allowed || scene == 0 || scene > 65535 || m_nativeInFlight ||
        m_pendingNativeContext ||
        (withCheckpoint && m_context.transition != MatchTransition::Shop &&
         m_nativeCompleted == 0) ||
        withCheckpoint != (party != nullptr) ||
        (challengeWon && transition != MatchTransition::SecretReturn) ||
        (transition == MatchTransition::Shop) != (shop != nullptr) ||
        (shop != nullptr && (!shop->valid() || shop->source != scene)) ||
        m_context.epoch == std::numeric_limits<u64>::max()) {
        return false;
    }
    if (m_context.transition == MatchTransition::Shop &&
        (transition != MatchTransition::Travel || !m_shop.finished())) {
        return false;
    }
    std::vector<std::vector<u8>> profiles;
    if (party != nullptr) {
        for (usize seat = 0; seat < m_owners.size(); ++seat) {
            if ((*party)[seat].has_value() != (m_owners[seat] != 0)) {
                return false;
            }
            if ((*party)[seat]) {
                auto packet = PartyCheckpointPacket::encode(
                    {m_context.epoch + 1, static_cast<u8>(seat), *(*party)[seat]});
                if (!packet) {
                    return false;
                }
                profiles.push_back(std::move(*packet));
            }
        }
    }
    const auto peers = static_cast<usize>(
        std::ranges::count_if(m_links, [](auto connection) { return connection != 0; }));
    if (m_controls.size() + peers * (1 + profiles.size()) > kControlBudget) {
        return false;
    }
    // Copy before replacing internal storage: callers may reuse travelParty().
    MatchParty nextParty = party != nullptr ? *party : MatchParty{};
    const auto previousContext = m_context;
    const auto finalTick = m_nativeCompleted;
    auto drainingInputs = withCheckpoint ? std::move(m_lockstep) : nullptr;
    m_context = {};
    m_context.epoch = previousContext.epoch + 1;
    m_context.scene = scene;
    m_context.transition = transition;
    m_context.challengeWon = challengeWon;
    m_context.owners = m_owners;
    m_context.inputLead = m_startupDelay;
    if (withCheckpoint) {
        m_context.sourceEpoch = previousContext.epoch;
        m_context.sourceTick = finalTick;
    }
    m_travelParty = std::move(nextParty);
    for (usize seat = 0; seat < m_owners.size(); ++seat) {
        // Membership is fixed for this transport lifetime. Epochs, not grant
        // changes, isolate successive scenes; the original assigned grant is 1.
        m_context.grants[seat] = m_owners[seat] == 0 ? 0 : 1;
    }
    discardGameplay();
    if (!beginNativeInputs()) {
        stop(MatchStop::TransportFailure, true);
        return false;
    }
    m_drainingInputs = std::move(drainingInputs);
    if (m_drainingInputs) {
        m_drainingContext = previousContext;
    }
    m_shop.begin(m_context.epoch, m_local, m_owners, m_context.grants, shop);
    m_loaded = {};
    m_movieSkipVotes = 0;
    m_movieSkipRequests = 0;
    m_loadingSeconds = 0;
    m_phase = Phase::Loading;
    if (!broadcast(MatchControlKind::Prepare)) {
        stop(MatchStop::TransportFailure, true);
        return false;
    }
    for (const auto connection : m_links) {
        if (connection != 0) {
            for (const auto& profile : profiles) {
                m_controls.push_back({connection, profile});
            }
        }
    }
    return true;
}
const MatchParty* MatchSession::travelParty() const {
    if (!checkpointed(m_context.transition) ||
        (m_phase != Phase::Loading && m_phase != Phase::Running)) {
        return nullptr;
    }
    for (usize seat = 0; seat < m_owners.size(); ++seat) {
        if (m_travelParty[seat].has_value() != (m_owners[seat] != 0)) {
            return nullptr;
        }
    }
    return &m_travelParty;
}
MatchSession::Admission MatchSession::checkpoint(u8 peer, const PartyCheckpoint& message) {
    if (host() || peer != 1) {
        return Admission::WrongPeer;
    }
    if (m_pendingNativeContext && message.epoch == m_pendingNativeContext->epoch) {
        if (m_owners[message.seat] == 0) {
            return Admission::Invalid;
        }
        auto& profile = m_pendingNativeParty[message.seat];
        if (profile) {
            return CharacterProfilePacket::encode(*profile) ==
                           CharacterProfilePacket::encode(message.profile)
                       ? Admission::Accepted
                       : Admission::Invalid;
        }
        profile = message.profile;
        return Admission::Accepted;
    }
    if (message.epoch < m_context.epoch) {
        return Admission::Stale;
    }
    if (message.epoch != m_context.epoch || !checkpointed(m_context.transition) ||
        m_owners[message.seat] == 0) {
        return Admission::Invalid;
    }
    auto& profile = m_travelParty[message.seat];
    if (profile) {
        return CharacterProfilePacket::encode(*profile) ==
                       CharacterProfilePacket::encode(message.profile)
                   ? Admission::Accepted
                   : Admission::Invalid;
    }
    if (m_phase != Phase::Loading || m_loaded[m_local]) {
        return Admission::WrongPhase;
    }
    profile = message.profile;
    return Admission::Accepted;
}
void MatchSession::tryCommit() {
    if (!host() || m_phase != Phase::Loading) {
        return;
    }
    for (const auto owner : m_owners) {
        if (owner != 0 && !m_loaded[owner]) {
            return;
        }
    }
    if (!broadcast(MatchControlKind::Commit)) {
        stop(MatchStop::TransportFailure, true);
        return;
    }
    m_drainingInputs.reset();
    m_drainingContext.reset();
    m_phase = Phase::Running;
}
bool MatchSession::loaded() {
    if (m_phase != Phase::Loading || m_loaded[m_local] ||
        (checkpointed(m_context.transition) && travelParty() == nullptr) ||
        (m_context.transition == MatchTransition::Shop && m_shop.setup() == nullptr)) {
        return false;
    }
    m_loaded[m_local] = true;
    if (host()) {
        tryCommit();
    } else if (!queue(1, MatchControlKind::Ready)) {
        stop(MatchStop::TransportFailure, false);
        return false;
    }
    return true;
}
bool MatchSession::requestMovieSkip(u8 seat) {
    if (m_phase != Phase::Loading || !hasEntryMovie(m_context.transition) ||
        seat >= m_owners.size() || m_owners[seat] != m_local) {
        return false;
    }
    const auto bit = static_cast<u8>(1U << seat);
    if ((m_movieSkipRequests & bit) != 0) {
        return true;
    }
    const auto votes = static_cast<u8>(m_movieSkipVotes | bit);
    if (host() ? !broadcast(MatchControlKind::MovieSkipVote, MatchStop::None, votes)
               : !queue(1, MatchControlKind::MovieSkipVote, MatchStop::None, bit)) {
        stop(MatchStop::TransportFailure, true);
        return false;
    }
    m_movieSkipRequests |= bit;
    if (host()) {
        m_movieSkipVotes = votes;
    }
    return true;
}
bool MatchSession::movieSkipReady() const {
    const auto required = seatsOf(m_owners);
    return m_phase == Phase::Loading && hasEntryMovie(m_context.transition) && required != 0 &&
           m_movieSkipVotes == required;
}
void MatchSession::stop(MatchStop reason, bool notify) {
    if (m_phase == Phase::Offline || m_phase == Phase::Stopped) {
        return;
    }
    discardGameplay();
    m_controls.clear();
    m_shop.begin(0, 0, {}, {});
    m_phase = Phase::Stopped;
    m_stop = reason;
    if (notify && m_context.valid()) {
        if (host()) {
            broadcast(MatchControlKind::Stop, reason);
        } else {
            queue(1, MatchControlKind::Stop, reason);
        }
    }
}
void MatchSession::leave() {
    stop(MatchStop::Left, true);
}
void MatchSession::disconnected(PacketTransport::Connection connection) {
    const auto peer = peerOf(connection);
    if (peer == 0) {
        return;
    }
    if (m_nativeAgreement) {
        m_nativeAgreement->disconnected(connection);
    }
    m_links[peer] = 0;
    std::erase_if(m_controls, [&](const auto& queued) { return queued.connection == connection; });
    if (m_nativeAgreement) {
        failNativeAgreement(NativeAgreement::Failure::Disconnected);
    } else {
        stop(MatchStop::Disconnected, host());
    }
}
void MatchSession::update(f64 seconds, Time now) {
    if (m_nativeAgreement && !m_nativeAgreement->update(now)) {
        failNativeAgreement(NativeAgreement::Failure::Timeout);
        return;
    }
    if ((m_phase == Phase::Loading || m_pendingNativeContext) && std::isfinite(seconds) &&
        seconds > 0) {
        m_loadingSeconds += seconds;
        if (m_loadingSeconds >= kLoadTimeout) {
            stop(MatchStop::LoadTimeout, true);
        }
    }
}
MatchSession::Admission MatchSession::control(u8 peer, const MatchControl& message) {
    if (message.context.inputLead != m_startupDelay) {
        return Admission::Invalid;
    }
    // A host can abort before its queued Prepare reaches a guest. Stop is final,
    // not a request to start that future epoch or load its scene.
    if (!host() && peer == 1 && message.kind == MatchControlKind::Stop &&
        message.context.owners == m_owners && message.context.epoch >= m_context.epoch) {
        stop(message.stop, false);
        return Admission::Accepted;
    }
    if (host() && m_drainingContext && message.kind == MatchControlKind::Stop &&
        message.context == *m_drainingContext) {
        stop(message.stop == MatchStop::TransportFailure ? MatchStop::TransportFailure
                                                         : MatchStop::Left,
             true);
        return Admission::Accepted;
    }
    if (message.kind == MatchControlKind::Prepare) {
        if (host() || peer != 1) {
            return Admission::WrongPeer;
        }
        if (message.context.epoch <= m_context.epoch) {
            return Admission::Stale;
        }
        if (message.context.owners != m_owners) {
            return Admission::Invalid;
        }
        if (m_pendingNativeContext) {
            return message.context == *m_pendingNativeContext ? Admission::Stale
                                                              : Admission::Invalid;
        }
        const auto transition = message.context.transition;
        const bool allowed = (transition == MatchTransition::Start && m_phase == Phase::Lobby) ||
                             (checkpointed(transition) && m_phase == Phase::Running);
        if (!allowed || (m_context.transition == MatchTransition::Shop &&
                         transition != MatchTransition::Travel)) {
            return Admission::WrongPhase;
        }
        if (checkpointed(transition)) {
            if (message.context.sourceEpoch != m_context.epoch ||
                message.context.sourceTick < m_nativeCompleted ||
                message.context.sourceTick - m_nativeCompleted > LockstepInputs::kWindow ||
                (m_context.transition == MatchTransition::Shop ? message.context.sourceTick != 0
                                                               : message.context.sourceTick == 0) ||
                (m_nativeInFlight && *m_nativeInFlight > message.context.sourceTick)) {
                return Admission::Invalid;
            }
            m_pendingNativeContext = message.context;
            m_pendingNativeParty = {};
            m_loadingSeconds = 0;
            if (transition == MatchTransition::Shop) {
                m_pendingNativeShop = std::make_unique<ShopSync>();
                m_pendingNativeShop->begin(message.context.epoch, m_local, m_owners,
                                           message.context.grants);
            }
            // Even an already-executed boundary needs the scene adapter's
            // terminal-outcome check before acknowledging travel. A Shop has
            // no native scene to execute and retains its ordinary barrier.
            if (m_context.transition == MatchTransition::Shop) {
                return finishNativeDrain() ? Admission::Accepted : Admission::Invalid;
            }
            return Admission::Accepted;
        }
        return acceptPreparedContext(message.context) ? Admission::Accepted : Admission::Invalid;
    }
    if (m_pendingNativeContext && message.context == *m_pendingNativeContext && peer == 1 &&
        message.kind == MatchControlKind::MovieSkipVote) {
        m_pendingNativeMovieVotes |= message.movieSkipVotes;
        return Admission::Accepted;
    }
    if (message.context != m_context) {
        return Admission::Stale;
    }
    switch (message.kind) {
    case MatchControlKind::MovieSkipVote: {
        if (host() && (message.movieSkipVotes & ~seatsOf(m_owners, peer)) != 0) {
            return Admission::WrongPeer;
        }
        // A late vote may follow natural movie completion and the final Ready.
        if (m_phase != Phase::Loading) {
            return Admission::Stale;
        }
        const auto votes = static_cast<u8>(m_movieSkipVotes | message.movieSkipVotes);
        if (host() && votes != m_movieSkipVotes &&
            !broadcast(MatchControlKind::MovieSkipVote, MatchStop::None, votes)) {
            stop(MatchStop::TransportFailure, true);
            return Admission::Accepted;
        }
        m_movieSkipVotes = votes;
        return Admission::Accepted;
    }
    case MatchControlKind::Ready:
        if (!host()) {
            return Admission::WrongPeer;
        }
        if (m_phase != Phase::Loading) {
            return Admission::WrongPhase;
        }
        m_loaded[peer] = true;
        tryCommit();
        return Admission::Accepted;
    case MatchControlKind::Commit:
        if (host() || peer != 1) {
            return Admission::WrongPeer;
        }
        if (m_phase != Phase::Loading || !m_loaded[m_local]) {
            return Admission::WrongPhase;
        }
        m_phase = Phase::Running;
        return Admission::Accepted;
    case MatchControlKind::Pause:
    case MatchControlKind::RequestPause: return Admission::Invalid;
    case MatchControlKind::Stop:
        stop(host() && message.stop != MatchStop::TransportFailure ? MatchStop::Left : message.stop,
             host());
        return Admission::Accepted;
    case MatchControlKind::Prepare: break;
    }
    return Admission::Invalid;
}
MatchSession::Admission MatchSession::receive(PacketTransport::Connection connection,
                                              std::span<const u8> bytes, std::optional<Time> now) {
    const auto inputAt = now.value_or(Clock::now());
    const auto peer = peerOf(connection);
    if (peer == 0) {
        return Admission::WrongPeer;
    }
    if (m_phase == Phase::Stopped || m_phase == Phase::Offline) {
        return Admission::WrongPhase;
    }
    if (bytes.size() >= 4 && ByteReader(bytes).readU32() == kMagic) {
        const auto message = MatchControlPacket::decode(bytes);
        return message ? control(peer, *message) : Admission::Invalid;
    }
    if (NativeAgreement::recognizes(bytes)) {
        if (m_phase != Phase::Running || m_context.transition == MatchTransition::Shop) {
            return Admission::Stale;
        }
        const bool fresh = !m_nativeAgreement;
        if (!ensureNativeAgreement(inputAt)) {
            return Admission::Invalid;
        }
        const auto result = m_nativeAgreement->receive(connection, bytes, inputAt);
        if (m_nativeAgreement->failed()) {
            failNativeAgreement(NativeAgreement::Failure::Protocol);
            // A validated divergence notification is handled, not a request for
            // repair. Stop is already queued to terminate the whole shared match.
            return result == NativeAgreement::Admission::Invalid ? Admission::Invalid
                                                                 : Admission::Accepted;
        }
        switch (result) {
        case NativeAgreement::Admission::Accepted:
        case NativeAgreement::Admission::Duplicate: return Admission::Accepted;
        case NativeAgreement::Admission::Stale:
            if (fresh) {
                m_nativeAgreement.reset(); // late old epochs do not enable this epoch's observer
            }
            return Admission::Stale;
        case NativeAgreement::Admission::WrongPeer: return Admission::WrongPeer;
        case NativeAgreement::Admission::Full:
        case NativeAgreement::Admission::Failed:
            failNativeAgreement(NativeAgreement::Failure::Window);
            return Admission::Invalid;
        case NativeAgreement::Admission::Invalid: return Admission::Invalid;
        }
        return Admission::Invalid;
    }
    if (PartyCheckpointPacket::recognizes(bytes)) {
        const auto message = PartyCheckpointPacket::decode(bytes);
        return message ? checkpoint(peer, *message) : Admission::Invalid;
    }
    if (ShopSync::recognizes(bytes)) {
        if (!m_pendingNativeShop && m_context.transition != MatchTransition::Shop) {
            return Admission::Stale;
        }
        // A future setup may follow Prepare while the old scene still drains.
        // The separate bounded ShopSync cannot mutate its profiles or gameplay.
        auto& shop = m_pendingNativeShop ? *m_pendingNativeShop : m_shop;
        switch (shop.receive(peer, bytes)) {
        case ShopSync::Admission::Accepted: return Admission::Accepted;
        case ShopSync::Admission::Stale: return Admission::Stale;
        case ShopSync::Admission::WrongPeer: return Admission::WrongPeer;
        case ShopSync::Admission::Invalid: return Admission::Invalid;
        }
    }
    {
        const auto commands = InputPacket::decode(bytes);
        if (!commands) {
            return Admission::Invalid;
        }
        auto* inputs = m_lockstep.get();
        const auto* context = &m_context;
        std::optional<u64> endTick = nativeEndTick();
        if (host() && m_drainingInputs && m_drainingContext && !commands->empty() &&
            commands->front().epoch == m_drainingContext->epoch) {
            inputs = m_drainingInputs.get();
            context = &*m_drainingContext;
            endTick = m_context.sourceTick;
        } else if (!inputs || m_context.transition == MatchTransition::Shop ||
                   (m_phase != Phase::Running &&
                    !(m_phase == Phase::Loading && m_loaded[m_local]))) {
            return Admission::Stale;
        }
        for (const auto& command : *commands) {
            if (command.epoch != context->epoch || (endTick && command.tick > *endTick)) {
                return Admission::Stale;
            }
        }
        // Ready/Commit deliveries on different peer connections need not be
        // polled together. Retain current-epoch inputs after this peer loaded;
        // only Commit opens advance(). Never discard an accepted reliable edge.
        switch (inputs->receive(connection, bytes)) {
        case LockstepInputs::Admission::Accepted:
        case LockstepInputs::Admission::Duplicate: return Admission::Accepted;
        case LockstepInputs::Admission::Stale: return Admission::Stale;
        case LockstepInputs::Admission::WrongPeer: return Admission::WrongPeer;
        case LockstepInputs::Admission::Full:
        case LockstepInputs::Admission::Conflict:
            // A reliable message has already left the transport. Silently
            // dropping it on capacity/conflict would create an endless stall.
            stop(MatchStop::TransportFailure, true);
            return Admission::Invalid;
        case LockstepInputs::Admission::Invalid:
        case LockstepInputs::Admission::TooEarly: return Admission::Invalid;
        }
        return Admission::Invalid;
    }
}
bool MatchSession::inputReady() const {
    if (m_phase != Phase::Running || m_context.transition == MatchTransition::Shop ||
        inputTick() == std::numeric_limits<u64>::max()) {
        return false;
    }
    return !m_pendingNativeContext && !m_nativeInFlight && m_lockstep && m_lockstep->sampleReady();
}
bool MatchSession::sample(std::span<const InputCommand> frame) {
    m_sampled.clear();
    if (m_phase != Phase::Running || m_context.transition == MatchTransition::Shop ||
        inputTick() == std::numeric_limits<u64>::max() ||
        frame.size() != static_cast<usize>(std::ranges::count(m_owners, m_local))) {
        return false;
    }
    if (!inputReady() || !m_lockstep->sample(frame)) {
        return false;
    }
    m_sampled.assign(frame.begin(), frame.end());
    return true;
}
std::optional<LockstepInputs::Frame> MatchSession::advance() {
    if (m_phase != Phase::Running || m_context.transition == MatchTransition::Shop || !m_lockstep ||
        m_nativeInFlight ||
        (m_pendingNativeContext && m_lockstep->tick() > m_pendingNativeContext->sourceTick)) {
        return std::nullopt;
    }
    const auto tick = m_lockstep->tick();
    auto frame = m_lockstep->advance();
    if (frame) {
        m_nativeInFlight = tick;
        for (usize seat = 0; seat < m_owners.size(); ++seat) {
            if (m_owners[seat] != 0) {
                m_nativeResolved[seat] = (*frame)[seat];
            }
        }
    }
    return frame;
}
bool MatchSession::completedNativeTick(u64 tick) {
    if (m_phase != Phase::Running) {
        return false;
    }
    if (m_nativeInFlight) {
        if (*m_nativeInFlight != tick || m_nativeCompleted == std::numeric_limits<u64>::max() ||
            tick != m_nativeCompleted + 1) {
            return false;
        }
        m_nativeInFlight.reset();
        m_nativeCompleted = tick;
    } else if (!m_pendingNativeContext || m_nativeCompleted != tick ||
               m_pendingNativeContext->sourceTick != tick) {
        return false;
    }
    return !m_pendingNativeContext || tick != m_pendingNativeContext->sourceTick ||
           finishNativeDrain();
}
bool MatchSession::observeNative(u64 tick, const NativeAgreement::Digest& digest, Time now) {
    if (m_phase != Phase::Running || m_context.transition == MatchTransition::Shop ||
        m_nativeInFlight || tick == 0 || tick != m_nativeCompleted ||
        tick % kNativeAgreementPeriod != 0) {
        return false;
    }
    if (!ensureNativeAgreement(now)) {
        return false;
    }
    switch (m_nativeAgreement->submit(tick, digest, now)) {
    case NativeAgreement::Admission::Accepted:
    case NativeAgreement::Admission::Duplicate: return true;
    case NativeAgreement::Admission::Full:
        failNativeAgreement(NativeAgreement::Failure::Window, tick);
        return false;
    case NativeAgreement::Admission::Failed:
    case NativeAgreement::Admission::Stale:
    case NativeAgreement::Admission::WrongPeer:
    case NativeAgreement::Admission::Invalid:
        failNativeAgreement(NativeAgreement::Failure::Protocol, tick);
        return false;
    }
    return false;
}
void MatchSession::flush(PacketTransport& transport, Time now) {
    while (!m_controls.empty()) {
        const auto& control = m_controls.front();
        const auto result =
            transport.send(control.connection, control.bytes, PacketTransport::Delivery::Reliable);
        if (result == PacketTransport::SendResult::Congested) {
            return;
        }
        if (result != PacketTransport::SendResult::Sent) {
            const auto connection = control.connection;
            disconnected(connection);
            return;
        }
        m_controls.pop_front();
    }
    // Prepare may reach a guest before old input deliveries. Retain and drain
    // the source relay throughout Loading; Ready acknowledges execution of its
    // terminal tick, so tryCommit() is the only successful retirement boundary.
    if (m_drainingInputs && !m_drainingInputs->flush(transport)) {
        stop(MatchStop::TransportFailure, true);
        return;
    }
    if (m_context.transition == MatchTransition::Shop) {
        while (!m_shop.outgoing().empty()) {
            const auto& delivery = m_shop.outgoing().front();
            const auto result = transport.send(m_links[delivery.peer], delivery.bytes,
                                               PacketTransport::Delivery::Reliable);
            if (result == PacketTransport::SendResult::Congested) {
                return;
            }
            if (result != PacketTransport::SendResult::Sent) {
                disconnected(m_links[delivery.peer]);
                return;
            }
            m_shop.sent();
        }
        return;
    }
    if (m_phase != Phase::Running) {
        return;
    }
    // In particular, a congested Commit above must reach this destination
    // before any reliable frame. Native peers produce their own local audio
    // and presentation; no host snapshots or duplicate cue stream is sent.
    if (!m_lockstep || !m_lockstep->flush(transport)) {
        stop(MatchStop::TransportFailure, true);
        return;
    }
    if (m_nativeAgreement && !m_nativeAgreement->flush(transport, now)) {
        failNativeAgreement(NativeAgreement::Failure::Transport);
    }
    return;
}
} // namespace gdl::game
