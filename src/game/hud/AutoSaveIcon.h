#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "engine/core/Types.h"
#include "engine/render/Image.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"

namespace gdl::game {
/** Brief successful-save notification using the original animated memory-card rune. */
class AutoSaveIcon {
public:
    static constexpr f64 kDuration = 2.0;
    static constexpr f64 kFrameSeconds = 8.0 / 60.0;
    /** The native carddemo/icon.tpl is beside the Gauntlet asset directory. */
    bool load(RenderDevice& device, const std::filesystem::path& assetRoot);
    bool load(RenderDevice& device, std::span<const Image> frames);
    void release();
    void show();
    void update(f64 seconds);
    bool visible() const { return !m_frames.empty() && m_remaining > 0; }
    usize frame() const;
    usize frameCount() const { return m_frames.size(); }
    void render(RenderDevice& device, Extent2D extent) const;

private:
    std::vector<std::unique_ptr<Texture>> m_frames;
    f64 m_remaining = 0;
    f64 m_elapsed = 0;
};
} // namespace gdl::game
