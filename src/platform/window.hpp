#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct SDL_Window;
union SDL_Event;

namespace dayo::platform {

struct WindowOptions {
    std::string title { "mikumikudesu" };
    std::uint32_t width { 1280 };
    std::uint32_t height { 720 };
    bool hidden {};
};

struct WindowEvent {
    enum class Type { quit, resized, fileDropped, cameraDragged, cameraZoomed };
    Type type {};
    std::filesystem::path path;
    float x {};
    float y {};
};

class Window {
public:
    virtual ~Window() = default;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] virtual SDL_Window* sdlHandle() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t pixelWidth() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t pixelHeight() const noexcept = 0;
    [[nodiscard]] virtual bool minimized() const noexcept = 0;
    virtual std::vector<WindowEvent> pollEvents() = 0;
    virtual void setTitle(const std::string& title) = 0;

protected:
    Window() = default;
};

[[nodiscard]] std::unique_ptr<Window> createWindow(const WindowOptions& options);

} // namespace dayo::platform
