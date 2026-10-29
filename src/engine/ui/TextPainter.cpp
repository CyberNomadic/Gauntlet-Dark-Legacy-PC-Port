#include "engine/ui/TextPainter.h"

#include "engine/core/Assert.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

namespace gdl {

void TextPainter::setFont(const BitmapFont* font, const Texture* texture) {
    m_font = font;
    m_texture = texture;
}

s32 TextPainter::measure(std::string_view text, f32 scale) const {
    s32 width = 0;
    for (const char c : text) {
        width += advance(static_cast<u8>(c), scale);
    }
    return width;
}

s32 TextPainter::lineHeight(f32 scale) const {
    return m_font == nullptr ? 0 : static_cast<s32>(static_cast<f32>(m_font->height()) * scale);
}

s32 TextPainter::leftEdge(s32 x, std::string_view text, f32 scale) const {
    return x < 0 ? -x - measure(text, scale) / 2 : x;
}

s32 TextPainter::draw(Canvas& canvas, s32 x, s32 y, std::string_view text,
                      const TextStyle& style) const {
    GDL_VERIFY(ready(), "TextPainter::draw without a font");
    const Texture* texture = style.texture != nullptr ? style.texture : m_texture;
    const auto sheetWidth = static_cast<f32>(m_texture->width());
    const auto sheetHeight = static_cast<f32>(m_texture->height());
    const auto cellHeight = static_cast<f32>(m_font->height());
    const auto expand = static_cast<f32>(style.expand);

    s32 penX = leftEdge(x, text, style.scale);
    for (const char c : text) {
        const auto code = static_cast<u8>(c);
        const BitmapGlyph* glyph = m_font->glyph(code);
        if (glyph != nullptr && glyph->width > 0) {
            // A glyph cut from part of a cell draws that part where its cell places it.
            const auto columns =
                static_cast<f32>(glyph->columns > 0 ? glyph->columns : glyph->width);
            const auto rows = glyph->rows > 0 ? static_cast<f32>(glyph->rows) : cellHeight;
            const f32 inset = kCellInset * style.scale;
            const Rect area{static_cast<f32>(penX) + static_cast<f32>(glyph->lead) * style.scale +
                                inset - expand,
                            static_cast<f32>(y) + static_cast<f32>(glyph->top) * style.scale +
                                inset - expand,
                            (columns - 2.0f * kCellInset) * style.scale + 2.0f * expand,
                            (rows - 2.0f * kCellInset) * style.scale + 2.0f * expand};
            const Rect uv{(static_cast<f32>(glyph->x) + kCellInset) / sheetWidth,
                          (static_cast<f32>(glyph->y) + kCellInset) / sheetHeight,
                          (columns - 2.0f * kCellInset) / sheetWidth,
                          (rows - 2.0f * kCellInset) / sheetHeight};
            canvas.draw(*texture, area, uv, style.color);
        }
        penX += advance(code, style.scale);
    }
    return penX;
}

s32 TextPainter::advance(u8 code, f32 scale) const {
    if (m_font == nullptr) {
        return 0;
    }
    const BitmapGlyph* glyph = m_font->glyph(code);
    if (glyph != nullptr && glyph->width > 0) {
        return static_cast<s32>(static_cast<f32>(glyph->width) * scale);
    }
    if (code == ' ') {
        return static_cast<s32>(static_cast<f32>(m_font->spaceWidth()) * scale);
    }
    return 0;
}

void visitNativeState(NativeStateDigest& digest, const TextPainter& painter) {
    const auto& [font, texture] = painter;
    digest.string("text-painter/layout/1");
    digest.flag(font != nullptr);
    if (font != nullptr) {
        digest.value(*font);
    }
    // Texture presence controls readiness. Its pixels, dimensions and device
    // identity affect drawing only, never measure(), lineHeight() or paging.
    digest.flag(texture != nullptr);
}

} // namespace gdl
