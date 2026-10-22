#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "engine/render/RenderDevice.h"

#include "game/players/Party.h"
#include "game/screens/NetplayMenu.h"
#include "game/screens/SessionInputs.h"

namespace gdl::game {
struct GameContext;
struct LevelRef;
struct PlayOptions;
/** Online runtime shared by the menu and development launcher. Transport/admission are
 * independent of presentation; selected local characters remain owned by their machine.
 * Completed shops return authenticated local checkpoints to the application. */
class OnlineRun {
public:
    static bool available();
    OnlineRun();
    ~OnlineRun();
    OnlineRun(const OnlineRun&) = delete;
    OnlineRun& operator=(const OnlineRun&) = delete;
    OnlineRun(OnlineRun&&) = delete;
    OnlineRun& operator=(OnlineRun&&) = delete;
    bool open(RenderDevice& device, const GameContext& context, const std::string& endpoint,
              const std::string& code, const std::string& build, const std::string& content,
              std::span<const PartyMember> local, const LevelRef& initial,
              const PlayOptions& options, bool autoStart = false,
              const std::filesystem::path& invitationOutput = {});
    void update(const SessionInputs::Frame& devices, const MenuInput& menu);
    bool openLobby(RenderDevice& device, const GameContext& context, const std::string& invitation,
                   const std::string& build, const std::string& content, u8 localPlayers,
                   bool localOnly = false);
    bool select(std::span<const PartyMember> local);
    bool ready(bool value);
    bool start();
    bool settings(const RoomSettings& value);
    NetplayMenu::View lobby() const;
    bool playing() const;
    u64 simulatedTick() const;
    u64 confirmedTick() const;
    /** Links straight to other guests, bypassing the host for their inputs. */
    usize directLinks() const;
    bool pause(s32 device);
    void resume();
    bool hasCompletedProgress() const;
    bool saveCompletedProgress();
    std::optional<Vec3> cursorAim(s32 device, Vec2 cursor) const;
    void render(RenderDevice& device, const Mat4& projection, f32 width, f32 height, f64 seconds,
                f32 frameBlend);
    void close();
    const std::string& status() const { return m_status; }
    /** The line drawn in the corner while playing: ping, the share of ticks the rollback
     * has corrected, how far the shown tick runs ahead of the confirmed one, and whether
     * the slowest link goes through a relay. Unknown ping reads as such. */
    static std::string networkSummary(s32 pingMs, bool relayed, u64 nativeTicks, u64 corrections,
                                      u64 ticksAhead);
    const std::string& network() const { return m_network; }
    /** How long a simulation starved of a peer's inputs waits before the session ends. */
    static constexpr f64 kStallDeadlineSeconds = 15;
    /** Seconds a stall may last before the players are told about it. */
    static constexpr f64 kStallNoticeSeconds = 1;
    /** The status shown `stalledSeconds` into a stall: nothing at first, then the wait
     * and the seconds left before the deadline. */
    static std::optional<std::string> stallNotice(f64 stalledSeconds);

private:
    bool connect(RenderDevice& device, const GameContext& context, bool localOnly,
                 const std::string& invitation, const std::string& build,
                 const std::string& content, u8 localPlayers);
    void status(RenderDevice& device, std::string message);
    void network(RenderDevice& device, std::string line);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::unique_ptr<Texture> m_statusTexture;
    std::string m_status;
    std::unique_ptr<Texture> m_networkTexture;
    std::string m_network;
};
} // namespace gdl::game
