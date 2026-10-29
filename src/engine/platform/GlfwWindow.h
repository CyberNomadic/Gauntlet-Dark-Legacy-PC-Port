#pragma once

#include "engine/core/Types.h"
#include "engine/platform/Gamepads.h"
#include "engine/platform/Window.h"

struct GLFWwindow;
struct GLFWmonitor;
struct GLFWcursor;

namespace gdl {

class GlfwWindow final : public Window {
public:
    explicit GlfwWindow(const WindowDesc& desc);
    ~GlfwWindow() override;

    GDL_NON_COPYABLE_NON_MOVABLE(GlfwWindow);

    void pollEvents() override;
    bool shouldClose() const override;
    void requestClose() override;
    Extent2D framebufferSize() const override;
    u32 refreshRate() const override;
    DisplayOptions displayOptions() const override;
    WindowMode windowMode() const override;
    bool cursorCaptured() const override;
    bool setDisplayMode(WindowMode mode, Extent2D resolution) override;
    bool selectMonitor(s32 index) override;
    void waitWhileMinimized() override;
    const Input& input() const override { return m_input; }
    bool rumble(s32 pad, u16 low, u16 high, u32 milliseconds, u8 priority = 0) override {
        return m_gamepads.rumble(pad, low, high, milliseconds, priority);
    }
    void stopRumble() override { m_gamepads.stop(); }
    std::string clipboardText() const override;
    void setClipboardText(const std::string& text) override;
    void setIcon(std::span<const Image> images) override;
    bool setCursor(const Image& image, u32 hotX, u32 hotY) override;
    void resetCursor() override;
    void holdCursor(bool held) override;
    void hideCursor(bool hidden) override;

    GraphicsApi api() const override { return m_api; }
    std::vector<const char*> requiredVulkanInstanceExtensions() const override;
    bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface) const override;
    void makeContextCurrent(bool current) override;
    void swapBuffers() override;
    void setSwapInterval(s32 interval) override;
    OpenGlProcedure openGlProcedure(const char* name) const override;

private:
    void pollKeyboard();
    /** Sets GLFW's cursor mode from what holds, hides or confines the pointer now. */
    void applyCursorMode();
    GLFWmonitor* activeMonitor() const;
    /** The display chosen, while it is attached; else the one the window is on. */
    GLFWmonitor* targetMonitor() const;
    static void charCallback(GLFWwindow* window, u32 codepoint);
    static void keyCallback(GLFWwindow* window, s32 key, s32 scancode, s32 action, s32 mods);

    GLFWwindow* m_window = nullptr;
    GraphicsApi m_api = GraphicsApi::Vulkan;
    GLFWcursor* m_cursor = nullptr;
    bool m_cursorHeld = false;
    bool m_cursorHidden = false;
    Input m_input;
    Gamepads m_gamepads;
    GLFWmonitor* m_desktopMonitor = nullptr;
    s32 m_preferredMonitor = -1; ///< the display chosen in the settings, by GLFW's order
    Extent2D m_desktopSize;
    s32 m_desktopRefresh = 60;
    s32 m_windowX = 100;
    s32 m_windowY = 100;
};

} // namespace gdl
