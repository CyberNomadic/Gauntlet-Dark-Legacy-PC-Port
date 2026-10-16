#include "game/netplay/FrameEventLedger.h"

namespace gdl::game {
bool FrameEventEmitter::valid() const {
    return nameSpace != 0 && generation != 0 && seat <= kNoSeat;
}
bool FrameEventIdentity::valid() const {
    return epoch != 0 && tick != 0 && emitter.valid() && cue != 0;
}
bool FrameLoopKey::valid() const {
    return emitter.valid() && channel != 0;
}
} // namespace gdl::game
