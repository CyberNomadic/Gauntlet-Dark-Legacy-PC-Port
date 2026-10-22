#include "game/rollback/PlayerHealthCheckpoint.h"

namespace gdl::game {
std::optional<std::vector<u8>>
PlayerHealthCheckpoint::encode(const PlayerHealth::Checkpoint& state) {
    return RandomCheckpoint::encode(state);
}
std::optional<PlayerHealth::Checkpoint> PlayerHealthCheckpoint::decode(std::span<const u8> bytes) {
    return RandomCheckpoint::decode(bytes);
}
} // namespace gdl::game
