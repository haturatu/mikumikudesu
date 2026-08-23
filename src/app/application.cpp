#include "app/application.hpp"

#include "core/asset.hpp"
#include "core/animation.hpp"
#include "core/denoiser.hpp"
#include "core/log.hpp"
#include "core/model_probe.hpp"
#include "core/motion.hpp"
#include "graphics/device.hpp"
#include "platform/window.hpp"

#if DAYO_HAS_IMGUI
#include <imgui.h>
#endif

#include <charconv>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
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
    device_ = device.get();
    device->selectRenderer(options_.renderer);
    log::info("Graphics convention: depth [0,1], Vulkan framebuffer Y handled in backend");
    const auto denoiser = core::selectDenoiser();
    log::info("Denoiser: ", denoiser.detail);
    log::info("Media: ", DAYO_HAS_MEDIA ? "FFmpeg available" : "FFmpeg development libraries not found");

    for (const auto& asset : options_.assets) handleAsset(asset);
    if (options_.probeOnly) {
        std::cout << device->capabilities().json() << '\n';
        return 0;
    }

    bool running = true;
    std::uint64_t frameCount = 0;
    auto previousTick = std::chrono::steady_clock::now();
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
        const auto tick = std::chrono::steady_clock::now();
        const float deltaSeconds = std::chrono::duration<float>(tick - previousTick).count();
        previousTick = tick;
        if (playing_ && animator_ != nullptr && motion_ != nullptr && motion_->lastFrame > 0) {
            animationFrame_ = std::fmod(animationFrame_ + deltaSeconds * 30.0F,
                                        static_cast<float>(motion_->lastFrame + 1));
            const int integerFrame = static_cast<int>(animationFrame_);
            if (integerFrame != uploadedAnimationFrame_) refreshAnimatedMesh(false);
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
            model_ = std::make_unique<core::PmxModel>(core::loadPmxModel(path));
            animator_ = std::make_unique<core::MmdAnimator>(*model_);
            animator_->setMotion(motion_.get());
            animator_->setPose(pose_.get());
            animationFrame_ = 0.0F;
            uploadedAnimationFrame_ = -1;
            refreshAnimatedMesh(true);
            lastAsset_ = "PMX " + model_->metadata.modelName + " — v"
                       + std::to_string(model_->metadata.version) + ", vertices "
                       + std::to_string(model_->metadata.vertexCount) + ", triangles "
                       + std::to_string(model_->indices.size() / 3) + ", bones "
                       + std::to_string(model_->bones.size());
            log::info("Loaded metadata: ", lastAsset_, " (", path.string(), ")");
        } catch (const std::exception& exception) {
            lastAsset_ = "PMX error: " + std::string(exception.what());
            log::warn(lastAsset_);
        }
        return;
    }
    if (kind == core::AssetKind::vmd) {
        try {
            motion_ = std::make_unique<core::VmdMotion>(core::loadVmd(path));
            if (animator_ != nullptr) { animator_->setMotion(motion_.get()); animationFrame_ = 0.0F; refreshAnimatedMesh(false); }
            lastAsset_ = "VMD " + motion_->modelName + " — " + std::to_string(motion_->bones.size())
                       + " bone keys, " + std::to_string(motion_->morphs.size()) + " morph keys, "
                       + std::to_string(motion_->lastFrame) + " frames";
            log::info("Loaded motion: ", lastAsset_, " (", path.string(), ")");
        } catch (const std::exception& exception) {
            lastAsset_ = "VMD error: " + std::string(exception.what());
            log::warn(lastAsset_);
        }
        return;
    }
    if (kind == core::AssetKind::vpd) {
        try {
            pose_ = std::make_unique<core::VpdPose>(core::loadVpd(path));
            if (animator_ != nullptr) { animator_->setPose(pose_.get()); refreshAnimatedMesh(false); }
            lastAsset_ = "VPD pose — " + std::to_string(pose_->bones.size()) + " bones";
            log::info("Loaded pose: ", lastAsset_, " (", path.string(), ")");
        } catch (const std::exception& exception) {
            lastAsset_ = "VPD error: " + std::string(exception.what());
            log::warn(lastAsset_);
        }
        return;
    }
    lastAsset_ = std::string(core::toString(kind)) + ": " + path.filename().string();
    log::info("Accepted ", core::toString(kind), " asset: ", path.string());
}

void Application::refreshAnimatedMesh(bool initialUpload) {
    if (device_ == nullptr || model_ == nullptr || animator_ == nullptr) return;
    auto frame = animator_->evaluate(animationFrame_);
    core::normalizeForPreview(frame.vertices, *model_);
    std::vector<graphics::PreviewVertex> vertices(frame.vertices.size());
    for (std::size_t i = 0; i < frame.vertices.size(); ++i) {
        std::memcpy(vertices[i].position, frame.vertices[i].position.data(), sizeof(vertices[i].position));
        std::memcpy(vertices[i].normal, frame.vertices[i].normal.data(), sizeof(vertices[i].normal));
        std::memcpy(vertices[i].uv, frame.vertices[i].uv.data(), sizeof(vertices[i].uv));
    }
    if (initialUpload) device_->uploadPreviewMesh(vertices, model_->indices);
    else device_->updatePreviewVertices(vertices);
    std::vector<graphics::PreviewMaterial> materials;
    materials.reserve(model_->materials.size());
    std::uint32_t firstIndex = 0;
    for (std::size_t i = 0; i < model_->materials.size(); ++i) {
        graphics::PreviewMaterial material;
        material.firstIndex = firstIndex;
        material.indexCount = model_->materials[i].indexCount;
        material.doubleSided = (model_->materials[i].drawFlags & 0x01U) != 0;
        const auto& diffuse = i < frame.materialDiffuse.size() ? frame.materialDiffuse[i]
                                                               : model_->materials[i].diffuse;
        std::copy(diffuse.begin(), diffuse.end(), material.diffuse);
        materials.push_back(material);
        firstIndex += material.indexCount;
    }
    device_->updatePreviewMaterials(materials);
    uploadedAnimationFrame_ = static_cast<int>(animationFrame_);
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
        if (motion_ != nullptr) {
            ImGui::Checkbox("Play", &playing_);
            float maximum = static_cast<float>(std::max(motion_->lastFrame, 1U));
            if (ImGui::SliderFloat("Frame", &animationFrame_, 0.0F, maximum, "%.1f")) {
                refreshAnimatedMesh(false);
            }
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Subayai and BDPT are enabled only when Vulkan RT features are present.");
    }
    ImGui::End();
#endif
}

} // namespace dayo::app
