#pragma once

#include <span>
#include <vector>

#include "formats/LegendsMap.h"

namespace gdl::formats {

/** Decode v1 Dreamcast bitmap slots and native textures.rom pixels.
 * Supports RGB565/ARGB1555/ARGB4444, twiddled/rectangular and normal/small VQ,
 * including the largest authored mip. Borrowed slots keep empty images.
 * Unsupported encodings and malformed buffers throw FormatError. */
std::vector<LegendsMapTexture> decodeLegendsDreamcastTextures(std::span<const u8> models,
                                                              std::span<const u8> pixels);

} // namespace gdl::formats
