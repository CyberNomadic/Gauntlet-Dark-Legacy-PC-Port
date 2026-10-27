#include "engine/app/Application.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <format>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/platform/DisplayTiming.h"
#include "engine/platform/Paths.h"
#include "engine/render/ThreadedRenderDevice.h"

namespace gdl {
namespace {
bool frameTimingEnabled() {
    const char* value = std::getenv("GDL_FRAME_TIMING");
    return value != nullptr && std::string_view(value) == "1";
}

std::optional<SpikeThreshold> spikeThresholdFromEnvironment() {
    const char* value = std::getenv("GDL_SPIKE_MS");
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto parsed = SpikeThreshold::parse(value);
    if (!parsed.has_value()) {
        log::warn("GDL_SPIKE_MS={} is not off, auto or a number of milliseconds", value);
    }
    return parsed;
}

void reportFrameTiming(const FrameTimingTotals& totals, u32 frameRate) {
    log::info("Frame timing: frames={} cap={} fps={:.2f} updates/frame={:.3f} overruns={}",
              totals.frames, frameRate, totals.framesPerSecond(),
              static_cast<f64>(totals.updates) / totals.frames, totals.overruns);
    using Phase = FrameTimingPhase;
    log::info("Frame timing ms avg/max: poll={:.3f}/{:.3f} update={:.3f}/{:.3f} "
              "acquire={:.3f}/{:.3f} render={:.3f}/{:.3f} present={:.3f}/{:.3f} "
              "refresh={:.3f}/{:.3f} wait={:.3f}/{:.3f} oversleep={:.3f}/{:.3f}",
              totals.average(Phase::Poll), totals.maximum(Phase::Poll),
              totals.average(Phase::Update), totals.maximum(Phase::Update),
              totals.average(Phase::Acquire), totals.maximum(Phase::Acquire),
              totals.average(Phase::Render), totals.maximum(Phase::Render),
              totals.average(Phase::Present), totals.maximum(Phase::Present),
              totals.average(Phase::Refresh), totals.maximum(Phase::Refresh),
              totals.average(Phase::Wait), totals.maximum(Phase::Wait),
              totals.average(Phase::Oversleep), totals.maximum(Phase::Oversleep));
}
} // namespace

Application::Application(ApplicationDesc desc) : m_desc(std::move(desc)) {}

Application::~Application() = default;

std::string FrameSpike::describe() const {
    using Phase = FrameTimingPhase;
    const auto ms = [this](Phase phase) {
        return timing.milliseconds[static_cast<usize>(phase)];
    };
    return std::format("Frame spike: {:.1f} ms of work past {:.1f} at frame {}: poll {:.1f} "
                       "update {:.1f} ({} ticks) acquire {:.1f} render {:.1f} present {:.1f}",
                       busyMilliseconds, thresholdMilliseconds, frame, ms(Phase::Poll),
                       ms(Phase::Update), timing.updates, ms(Phase::Acquire), ms(Phase::Render),
                       ms(Phase::Present));
}

void Application::onFrameSpike(const FrameSpike& spike) {
    log::warn("{}", spike.describe());
}

void Application::setUpdateRate(u32 rate) {
    if (rate != m_desc.updateRate) {
        m_desc.updateRate = rate;
        // A fraction accumulated in an offline clock must not become a burst
        // of online updates (or vice versa) when entering/leaving a room.
        m_updateClock = {};
    }
}

s32 Application::run() {
    try {
        log::info("Starting {}", m_desc.window.title);
        m_window = createGlfwWindow(m_desc.window);

        RenderDeviceDesc deviceDesc;
        deviceDesc.vsync = m_desc.vsync;
        deviceDesc.sampleCount = m_desc.sampleCount;
        deviceDesc.textureFiltering = m_desc.textureFiltering;
        deviceDesc.enableValidation = m_desc.enableValidation;
        deviceDesc.shaderDirectory = paths::executableDirectory() / "shaders";
        m_device = createVulkanRenderDevice(*m_window, deviceDesc);
        if (m_desc.threadedPresentation) {
            m_device = std::make_unique<ThreadedRenderDevice>(std::move(m_device),
                                                              m_window->framebufferSize());
        }

        checkAssetDirectory();
        if (const auto threshold = spikeThresholdFromEnvironment()) {
            m_spikeThreshold = *threshold;
        }
        onInit();
        u32 lastFrameRate = 0;
        FramePacer framePacer;
        const bool measureFrames = frameTimingEnabled();
        FrameTimingTotals timingTotals;
        UpdateClock presentationClock;
        bool renderDue = true;
        f64 renderElapsed = 0;

        while (!m_window->shouldClose() && !m_quitRequested) {
            const auto frameStart = std::chrono::steady_clock::now();
            auto mark = frameStart;
            FrameTimingSample timing;
            // The spike tracer measures too: a few clock reads a frame, reported only when
            // a frame's work runs past its threshold.
            const bool traceSpikes =
                m_spikeThreshold.at(lastFrameRate != 0 ? lastFrameRate : 60) > 0;
            const auto measure = [&](FrameTimingPhase phase) {
                if (measureFrames || traceSpikes) {
                    const auto now = std::chrono::steady_clock::now();
                    timing.measure(phase, mark, now);
                    mark = now;
                }
            };
            m_window->pollEvents();
            m_clock.tick();
            renderElapsed += m_clock.deltaSeconds();
            measure(FrameTimingPhase::Poll);
            if (m_desc.updateRate == 0) {
                onUpdate(m_clock.deltaSeconds());
                ++timing.updates;
            } else {
                m_updateInput.accumulate(m_window->input());
                const u32 rate = m_desc.updateRate;
                const u32 ticks = m_updateClock.advance(m_clock.deltaSeconds(), rate);
                for (u32 tick = 0; tick < ticks && !m_quitRequested; ++tick) {
                    onUpdate(1.0 / rate);
                    m_updateInput.beginPoll();
                    ++timing.updates;
                    if (m_desc.updateRate != rate) {
                        break; // The remaining ticks belonged to the previous clock.
                    }
                }
            }
            measure(FrameTimingPhase::Update);

            const u32 refreshRate = m_window->refreshRate();
            const u32 frameRate = displayFrameRate(m_desc.maxFrameRate, refreshRate);
            if (m_desc.threadedPresentation) {
                m_device->setFramebufferSize(m_window->framebufferSize());
                renderDue =
                    presentationClock.advance(m_clock.deltaSeconds(), frameRate) > 0 || renderDue;
            }
            const bool drawable =
                (!m_desc.threadedPresentation || renderDue) && m_device->beginFrame();
            measure(FrameTimingPhase::Acquire);
            if (drawable) {
                m_renderDelta = renderElapsed;
                onRender(*m_device);
                measure(FrameTimingPhase::Render);
                m_device->endFrame();
                renderElapsed = 0;
                renderDue = false;
                ++m_renderedFrames;
            } else if (!m_desc.threadedPresentation) {
                m_window->waitWhileMinimized();
            }
            measure(FrameTimingPhase::Present);

            measure(FrameTimingPhase::Refresh);
            if (frameRate != lastFrameRate) {
                log::info("Presentation cap: {} fps on a {} Hz display", frameRate, refreshRate);
                lastFrameRate = frameRate;
            }
            const auto frameFinished = std::chrono::steady_clock::now();
            if (traceSpikes) {
                const f64 busy =
                    std::chrono::duration<f64, std::milli>(frameFinished - frameStart).count();
                const f64 threshold = m_spikeThreshold.at(frameRate);
                if (threshold > 0 && busy > threshold) {
                    onFrameSpike({timing, busy, threshold, m_renderedFrames});
                }
            }
            // Rendering at 30 Hz must not batch 60 Hz online input/network ticks in pairs.
            // A busy or minimized renderer still polls and services the session normally.
            const u32 loopRate =
                m_desc.threadedPresentation ? std::max(frameRate, m_desc.updateRate) : frameRate;
            const auto deadline = framePacer.deadline(frameStart, frameFinished, loopRate);
            std::this_thread::sleep_until(deadline);
            if (measureFrames) {
                const auto wake = std::chrono::steady_clock::now();
                timing.measure(FrameTimingPhase::Wait, frameFinished, wake);
                timing.measure(FrameTimingPhase::Oversleep, deadline, wake);
                timing.elapsedMilliseconds =
                    std::chrono::duration<f64, std::milli>(wake - frameStart).count();
                timing.overran = deadline <= frameFinished;
                timingTotals.add(timing);
                if (timingTotals.frames == 120) {
                    reportFrameTiming(timingTotals, frameRate);
                    timingTotals = {};
                }
            }

            if (m_desc.maxFrames != 0 && m_renderedFrames >= m_desc.maxFrames) {
                log::info("Reached the requested frame limit ({} frames)", m_desc.maxFrames);
                requestQuit();
            }
        }

        m_device->waitIdle();
        onShutdown();
        m_device.reset();
        m_window.reset();
        log::info("Clean shutdown");
        return 0;
    } catch (const std::exception& e) {
        log::error("Unhandled exception: {}", e.what());
        return 1;
    }
}

void Application::checkAssetDirectory() const {
    const auto& dir = m_desc.assetDirectory;
    if (dir.empty()) {
        log::warn("No asset directory configured (pass --assets <dir>)");
        return;
    }
    const auto marker = dir / "STATIC" / "objects.ngc";
    std::error_code ec;
    if (std::filesystem::exists(marker, ec)) {
        log::info("Asset directory: {}", dir.string());
    } else {
        log::warn("Asset directory {} does not contain STATIC/objects.ngc", dir.string());
    }
}

} // namespace gdl
