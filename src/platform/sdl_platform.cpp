#include "platform/window.hpp"

#include "core/log.hpp"

#include <SDL3/SDL.h>
#if DAYO_HAS_IMGUI
#include <backends/imgui_impl_sdl3.h>
#endif

#include <stdexcept>
#include <utility>

namespace dayo::platform {
namespace {

class SdlLifetime {
public:
    SdlLifetime() {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
            throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
        }
        log::info("SDL ", SDL_GetVersion(), " initialized (video/audio/gamepad)");
    }
    ~SdlLifetime() { SDL_Quit(); }
};

class SdlWindow final : public Window {
public:
    explicit SdlWindow(const WindowOptions& options)
        : lifetime_(std::make_shared<SdlLifetime>()) {
        SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (options.hidden) flags |= SDL_WINDOW_HIDDEN;
        window_ = SDL_CreateWindow(options.title.c_str(),
                                   static_cast<int>(options.width),
                                   static_cast<int>(options.height), flags);
        if (window_ == nullptr) {
            throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        }
        updateSize();
    }

    ~SdlWindow() override {
        if (window_ != nullptr) SDL_DestroyWindow(window_);
    }

    SDL_Window* sdlHandle() const noexcept override { return window_; }
    std::uint32_t pixelWidth() const noexcept override { return width_; }
    std::uint32_t pixelHeight() const noexcept override { return height_; }
    bool minimized() const noexcept override { return minimized_; }

    std::vector<WindowEvent> pollEvents() override {
        std::vector<WindowEvent> result;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
#if DAYO_HAS_IMGUI
            ImGui_ImplSDL3_ProcessEvent(&event);
#endif
            switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                result.push_back({ WindowEvent::Type::quit, {} });
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_RESIZED:
                updateSize();
                result.push_back({ WindowEvent::Type::resized, {} });
                break;
            case SDL_EVENT_WINDOW_MINIMIZED:
                minimized_ = true;
                break;
            case SDL_EVENT_WINDOW_RESTORED:
                minimized_ = false;
                updateSize();
                result.push_back({ WindowEvent::Type::resized, {} });
                break;
            case SDL_EVENT_DROP_FILE:
                if (event.drop.data != nullptr) {
                    result.push_back({ WindowEvent::Type::fileDropped,
                                       std::filesystem::path(event.drop.data) });
                }
                break;
            default:
                break;
            }
        }
        return result;
    }

    void setTitle(const std::string& title) override {
        if (!SDL_SetWindowTitle(window_, title.c_str())) {
            log::warn("SDL_SetWindowTitle failed: ", SDL_GetError());
        }
    }

private:
    void updateSize() {
        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSizeInPixels(window_, &width, &height)) {
            log::warn("SDL_GetWindowSizeInPixels failed: ", SDL_GetError());
            return;
        }
        width_ = static_cast<std::uint32_t>(std::max(width, 0));
        height_ = static_cast<std::uint32_t>(std::max(height, 0));
    }

    std::shared_ptr<SdlLifetime> lifetime_;
    SDL_Window* window_ {};
    std::uint32_t width_ {};
    std::uint32_t height_ {};
    bool minimized_ {};
};

} // namespace

std::unique_ptr<Window> createWindow(const WindowOptions& options) {
    return std::make_unique<SdlWindow>(options);
}

} // namespace dayo::platform
