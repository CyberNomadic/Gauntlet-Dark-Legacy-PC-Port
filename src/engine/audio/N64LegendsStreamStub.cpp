#include "engine/audio/N64LegendsStream.h"

namespace gdl {

/** Built without the Legends sequencer (GDL_LEGENDS_AUDIO off): the stream never opens, and
 * a Legends map plays without its music. */
struct N64LegendsStream::Impl {};

N64LegendsStream::N64LegendsStream() = default;
N64LegendsStream::~N64LegendsStream() = default;

bool N64LegendsStream::open(const std::filesystem::path& /*nativeAudioDirectory*/,
                            u16 /*songId*/) {
    return false;
}

bool N64LegendsStream::opened() const {
    return false;
}

AudioStreamDesc N64LegendsStream::desc() const {
    return {};
}

bool N64LegendsStream::read(std::vector<f32>& /*out*/, usize /*frames*/) {
    return false;
}

void N64LegendsStream::rewind() {}

} // namespace gdl
