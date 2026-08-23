#include "app/application.hpp"

#include "core/asset.hpp"
#include "core/log.hpp"
#include "core/model_probe.hpp"
#include "graphics/device.hpp"
#include "platform/window.hpp"

#if DAYO_HAS_IMGUI
#include <imgui.h>
#endif

#include <charconv>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace dayo::app {
namespace {

graphics::RendererKind parseRenderer(std::string_view value) {
    if (value == "preview") return graphics::RendererKind::preview;
    if (value == "subayai") return graphics::RendererKind::subayai;
    if (value == "bdpt") return graphics::RendererKind::bdpt;
    throw std::invalid_argument("unknown renderer: " + std::string(value));
}

std::uint64_t parseFrameCount(std::string_view value) {
    std::uint64_t frames = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), frames);
    if (error != std::errc {} || end != value.data() + value.size() || frames == 0) {
        throw std::invalid_argument("--frames expects a positive integer");
    }
    return frames;
}

} // namespace

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--probe") {
            options.probeOnly = true;
        } else if (argument == "--hidden") {
            options.hidden = true;
        } else if (argument == "--no-validation") {
            options.validation = false;
        } else if (argument == "--renderer") {
            if (++i >= argc) throw std::invalid_argument("--renderer requires a value");
            options.renderer = parseRenderer(argv[i]);
        } else if (argument == "--frames") {
            if (++i >= argc) throw std::invalid_argument("--frames requires a value");
            options.frameLimit = parseFrameCount(argv[i]);
        } else if (argument == "--model" || argument == "--asset") {
            if (++i >= argc) throw std::invalid_argument(std::string(argument) + " requires a path");
            options.assets.emplace_back(argv[i]);
        } else if (argument == "--help" || argument == "-h") {
            log::info("Usage: mikumikudesu [--probe] [--hidden] [--frames N] "
                      "[--renderer preview|subayai|bdpt] [--asset PATH] [--no-validation]");
            options.probeOnly = true;
        } else if (!argument.empty() && argument.front() == '-') {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        } else {
            options.assets.emplace_back(argument);
        }
    }
    return options;
}

Application::Application(Options options) : options_(std::move(options)) {}

int Application::run() {
    platform::WindowOptions windowOptions;
    windowOptions.title = "mikumikudesu — SDL3 + Vulkan";
    windowOptions.hidden = options_.hidden || options_.probeOnly;
    auto window = platform::createWindow(windowOptions);
    auto device = graphics::createVulkanDevice(*window, options_.validation);
    device->selectRenderer(options_.renderer);
    log::info("Graphics convention: depth [0,1], Vulkan framebuffer Y handled in backend");
    log::info("OIDN: ", DAYO_HAS_OIDN ? "available (CPU; HIP selectable at runtime)" : "not found, disabled");
    log::info("Media: ", DAYO_HAS_MEDIA ? "FFmpeg available" : "FFmpeg development libraries not found");

    for (const auto& asset : options_.assets) handleAsset(asset);
    if (options_.probeOnly) {
        std::cout << device->capabilities().json() << '\n';
        return 0;
    }

    bool running = true;
    std::uint64_t frameCount = 0;
    while (running) {
        for (const auto& event : window->pollEvents()) {
            switch (event.type) {
            case platform::WindowEvent::Type::quit:
                running = false;
                break;
            case platform::WindowEvent::Type::resized:
                device->resize();
                break;
            case platform::WindowEvent::Type::fileDropped:
                handleAsset(event.path);
                break;
            }
        }
        if (!running) break;
        if (window->minimized() || window->pixelWidth() == 0 || window->pixelHeight() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        device->beginUiFrame();
        buildUi();
        device->renderFrame();
        ++frameCount;
        if (options_.frameLimit && frameCount >= *options_.frameLimit) running = false;
    }
    device->waitIdle();
    log::info("Rendered ", frameCount, " frame(s); clean shutdown");
    return 0;
}

void Application::handleAsset(const std::filesystem::path& path) {
    const auto kind = core::classifyAsset(path);
    if (kind == core::AssetKind::unknown) {
        lastAsset_ = "Unsupported asset: " + path.string();
        log::warn(lastAsset_);
        return;
    }
    if (kind == core::AssetKind::pmx) {
        try {
            const auto metadata = core::probePmx(path);
            lastAsset_ = "PMX " + metadata.modelName + " — v" + std::to_string(metadata.version)
                       + ", vertices " + std::to_string(metadata.vertexCount);
            log::info("Loaded metadata: ", lastAsset_, " (", path.string(), ")");
        } catch (const std::exception& exception) {
            lastAsset_ = "PMX error: " + std::string(exception.what());
            log::warn(lastAsset_);
        }
        return;
    }
    lastAsset_ = std::string(core::toString(kind)) + ": " + path.filename().string();
    log::info("Accepted ", core::toString(kind), " asset: ", path.string());
}

void Application::buildUi() {
#if DAYO_HAS_IMGUI
    ImGui::SetNextWindowPos({ 24.0F, 24.0F }, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({ 460.0F, 360.0F }, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("mikumikudesu Linux Preview")) {
        ImGui::TextUnformatted("SDL3 + Vulkan native backend");
        ImGui::Separator();
        ImGui::Text("Renderer request: %s", graphics::toString(options_.renderer).data());
        ImGui::TextWrapped("%s", lastAsset_.c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted("Files: PMX / VMD / VPD / PNG / JPEG / DDS / WAV / MP3 / M4A / MP4 / AVI");
        ImGui::Text("OIDN: %s", DAYO_HAS_OIDN ? "CPU available; HIP optional" : "not installed");
        ImGui::Text("Media: %s", DAYO_HAS_MEDIA ? "FFmpeg enabled" : "metadata/drop only");
        ImGui::Separator();
        ImGui::TextUnformatted("Subayai and BDPT are enabled only when Vulkan RT features are present.");
    }
    ImGui::End();
#endif
}

} // namespace dayo::app
