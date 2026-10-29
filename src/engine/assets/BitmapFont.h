#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <vector>

#include "engine/core/Types.h"
#include "engine/io/CanonicalDigestSink.h"

namespace gdl {

class NativeStateDigest;

/** One character cell: its code and where it sits in the font texture; height is the font's.
 * A glyph cut from part of another's cell draws `columns` by `rows` texels of the sheet at
 * (`lead`, `top`) within a cell `width` wide; zero columns or rows draw the whole cell. */
struct BitmapGlyph {
    s32 code = 0;
    s32 width = 0; ///< the cell's advance
    s32 x = 0;
    s32 y = 0;
    s32 lead = 0;
    s32 top = 0;
    s32 columns = 0;
    s32 rows = 0;
};

/** The cell table of one bitmap font; characters without a cell take up no space except ' '. */
class BitmapFont {
public:
    /** A character made from part of another's cell: `columns` by `rows` texels of `from`'s
     * cell, from (`left`, `top`) within it, drawn at (`lead`, `at`) within a new cell `width`
     * wide; for a mark the font's artists never drew, cut from a stroke they did. */
    struct Cut {
        s32 code = 0;
        s32 from = 0;
        s32 left = 0;
        s32 top = 0;
        s32 columns = 0;
        s32 rows = 0;
        s32 width = 0;
        s32 lead = 0;
        s32 at = 0;
    };

    static BitmapFont fromGlyphs(s32 height, s32 spaceWidth, std::vector<BitmapGlyph> glyphs);

    /** Reads a native .fnt or a legacy manifest; a sibling .fnt takes precedence. */
    bool load(const std::filesystem::path& file, s32 spaceWidth);

    bool loaded() const { return m_height > 0; }
    s32 height() const { return m_height; }
    s32 spaceWidth() const { return m_spaceWidth; }
    usize glyphCount() const { return m_glyphs.size(); }

    /** The cell for a character code, or nullptr when the font has none. */
    const BitmapGlyph* glyph(u8 code) const;

    /** Adds the character `cut` describes; false, adding nothing, when the font already has
     * it, lacks the glyph it is cut from, or the part lies outside either cell. */
    bool addCut(const Cut& cut);

private:
    friend void visitNativeState(NativeStateDigest& /*digest*/, const BitmapFont& /*font*/);
    void index();

    s32 m_height = 0;
    s32 m_spaceWidth = 0;
    std::vector<BitmapGlyph> m_glyphs;
    std::array<s16, 256> m_lookup{};
    mutable std::optional<CanonicalDigestSink::Digest> m_nativeContent;
};

void visitNativeState(NativeStateDigest& /*digest*/, const BitmapFont& /*font*/);

} // namespace gdl
