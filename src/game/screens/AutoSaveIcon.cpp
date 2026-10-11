#include "game/screens/AutoSaveIcon.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/ui/Canvas.h"

#include "formats/TplFile.h"

namespace gdl::game {
bool AutoSaveIcon::load(RenderDevice& device, const std::filesystem::path& assetRoot) {
    release();
    const auto file = AssetLocator(assetRoot.parent_path()).find("carddemo/icon.tpl");
    if (!file) {
        log::warn("Autosave icon: carddemo/icon.tpl is unavailable");
        return false;
    }
    try {
        std::vector<Image> frames;
        for (auto& entry : formats::parseTplFile(readFile(*file))) {
            frames.push_back(std::move(entry.image));
        }
        return load(device, frames);
    } catch (const std::exception& error) {
        log::warn("Autosave icon: {}", error.what());
        return false;
    }
}
bool AutoSaveIcon::load(RenderDevice& device, std::span<const Image> frames) {
    release();
    try {
        if (frames.empty() || frames.size() > 8) {
            throw FormatError("expected one to eight memory-card frames");
        }
        for (const auto& frame : frames) {
            if (frame.width == 0 || frame.height == 0 || frame.width > 256 || frame.height > 256 ||
                frame.pixels.size() != static_cast<usize>(frame.width) * frame.height * 4) {
                throw FormatError("invalid memory-card frame");
            }
            m_frames.push_back(device.createTexture(
                {frame.width, frame.height, TextureFilter::Linear, TextureWrap::ClampToEdge},
                frame.pixels));
        }
    } catch (const std::exception& error) {
        release();
        log::warn("Autosave icon: {}", error.what());
        return false;
    }
    return true;
}
void AutoSaveIcon::release() {
    m_frames.clear();
    m_remaining = 0;
    m_elapsed = 0;
}
void AutoSaveIcon::show() {
    m_remaining = kDuration;
    m_elapsed = 0;
}
void AutoSaveIcon::update(f64 seconds) {
    if (!visible() || !std::isfinite(seconds) || seconds < 0) {
        return;
    }
    const f64 step = std::min(seconds, m_remaining);
    m_remaining -= step;
    m_elapsed += step;
}
usize AutoSaveIcon::frame() const {
    constexpr f64 kClockEpsilon = 1e-9;
    return m_frames.empty()
               ? 0
               : static_cast<usize>(m_elapsed / kFrameSeconds + kClockEpsilon) % m_frames.size();
}
void AutoSaveIcon::render(RenderDevice& device, Extent2D extent) const {
    if (!visible() || extent.isZero()) {
        return;
    }
    constexpr f32 kCanvasHeight = 384;
    constexpr f32 kSize = 32;
    constexpr f32 kMargin = 8;
    const f32 width =
        kCanvasHeight * static_cast<f32>(extent.width) / static_cast<f32>(extent.height);
    Canvas canvas;
    canvas.begin(device, makeScreenProjection(width, kCanvasHeight),
                 {.depthWrite = false, .depthTest = false});
    canvas.draw(*m_frames[frame()], {width - kMargin - kSize, kMargin, kSize, kSize});
    canvas.end();
}
} // namespace gdl::game
