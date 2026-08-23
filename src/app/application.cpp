#include "app/application.hpp"

#include "core/asset.hpp"
#include "core/animation.hpp"
#include "core/denoiser.hpp"
#include "core/image.hpp"
#include "core/log.hpp"
#include "core/model_probe.hpp"
#include "core/motion.hpp"
#include "graphics/device.hpp"
#include "platform/window.hpp"

#if DAYO_HAS_IMGUI
#include <imgui.h>
#endif

#include <charconv>
#include <cctype>
#include <algorithm>
#include <array>
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
        } else if (argument == "--save-project") {
            if (++i >= argc) throw std::invalid_argument("--save-project requires a path");
            options.saveProject = std::filesystem::path(argv[i]);
        } else if (argument == "--help" || argument == "-h") {
            log::info("Usage: mikumikudesu [--probe] [--hidden] [--frames N] "
                      "[--renderer preview|subayai|bdpt] [--asset PATH] "
                      "[--save-project PATH] [--no-validation]");
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
    if (options_.saveProject) {
        core::DayoProject project;
        project.renderer = std::string(graphics::toString(device->activeRenderer()));
        project.frame = animationFrame_;
        project.playing = playing_;
        project.assets = projectAssets_;
        core::saveProject(*options_.saveProject, project);
        log::info("Saved project: ", options_.saveProject->string());
    }
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
            case platform::WindowEvent::Type::cameraDragged:
                cameraYaw_ += event.x * 0.008F;
                cameraPitch_ = std::clamp(cameraPitch_ + event.y * 0.008F, -1.5F, 1.5F);
                manualCamera_ = true;
                refreshPreviewScene();
                break;
            case platform::WindowEvent::Type::cameraZoomed:
                cameraDistance_ = std::clamp(cameraDistance_ * std::exp(-event.x * 0.12F), 0.4F, 30.0F);
                manualCamera_ = true;
                refreshPreviewScene();
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
        if (playing_ && motion_ != nullptr && motion_->lastFrame > 0) {
            animationFrame_ = std::fmod(animationFrame_ + deltaSeconds * 30.0F,
                                        static_cast<float>(motion_->lastFrame + 1));
            const int integerFrame = static_cast<int>(animationFrame_);
            if (integerFrame != uploadedAnimationFrame_ && animator_ != nullptr) {
                refreshAnimatedMesh(false, deltaSeconds);
            }
            refreshPreviewScene();
        }
        if (playing_ && videoMode_ && media_ != nullptr && media_->info().hasVideo) {
            mediaSeconds_ += deltaSeconds;
            if (media_->info().durationSeconds > 0.0 && mediaSeconds_ >= media_->info().durationSeconds) {
                mediaSeconds_ = std::fmod(mediaSeconds_, media_->info().durationSeconds);
                uploadedVideoFrame_ = -1;
                if (media_->info().hasAudio) audioPlayer_.play(media_->decodeAudio());
            }
            const auto videoFrame = static_cast<std::int64_t>(mediaSeconds_ * media_->info().videoFramesPerSecond);
            if (videoFrame != uploadedVideoFrame_) refreshVideoFrame();
        }
        device->beginUiFrame();
        buildUi();
        device->renderFrame();
        ++frameCount;
        if (options_.frameLimit && frameCount >= *options_.frameLimit) running = false;
    }
    device->waitIdle();
    audioPlayer_.stop();
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
    if (kind == core::AssetKind::project) {
        try {
            const auto project = core::loadProject(path);
            projectAssets_.clear();
            if (project.renderer == "subayai") device_->selectRenderer(graphics::RendererKind::subayai);
            else if (project.renderer == "bdpt") device_->selectRenderer(graphics::RendererKind::bdpt);
            else device_->selectRenderer(graphics::RendererKind::preview);
            for (const auto& asset : project.assets) handleAsset(asset.path);
            animationFrame_ = project.frame;
            playing_ = project.playing;
            if (animator_ != nullptr) refreshAnimatedMesh(false);
            lastAsset_ = "Project " + path.filename().string() + " — "
                       + std::to_string(project.assets.size()) + " assets";
            log::info("Loaded project: ", lastAsset_);
        } catch (const std::exception& exception) {
            lastAsset_ = "Project error: " + std::string(exception.what());
            log::warn(lastAsset_);
        }
        return;
    }
    if (kind == core::AssetKind::pmx) {
        try {
            videoMode_ = false;
            media_.reset();
            audioPlayer_.stop();
            model_ = std::make_unique<core::PmxModel>(core::loadPmxModel(path));
            normalization_ = core::previewNormalization(*model_);
            animator_ = std::make_unique<core::MmdAnimator>(*model_);
            physics_ = std::make_unique<core::MmdPhysics>(*model_);
            animator_->setMotion(motion_.get());
            animator_->setPose(pose_.get());
            animator_->setPhysics(physics_.get());
            textures_.clear();
            textures_.resize(model_->textures.size());
            std::size_t loadedTextures = 0;
            for (std::size_t i = 0; i < model_->textures.size(); ++i) {
                try {
                    auto texturePath = model_->textures[i];
                    if (!std::filesystem::exists(texturePath)) {
                        // Some legacy ZIP extractors rewrite Japanese filenames. A same-stem
                        // image beside the PMX is an unambiguous recovery for bundled assets.
                        for (const auto& entry : std::filesystem::directory_iterator(model_->sourcePath.parent_path())) {
                            if (!entry.is_regular_file() || entry.path().extension() != texturePath.extension()) continue;
                            if (entry.path().stem() == model_->sourcePath.stem()) { texturePath = entry.path(); break; }
                        }
                    }
                    textures_[i] = core::loadImageRgba8(texturePath);
                    ++loadedTextures;
                } catch (const std::exception& exception) {
                    log::warn(exception.what());
                }
            }
            std::vector<graphics::PreviewTexture> previewTextures;
            previewTextures.reserve(textures_.size());
            for (const auto& texture : textures_) {
                previewTextures.push_back({ texture.width, texture.height, texture.pixels });
            }
            if (device_ != nullptr) device_->uploadPreviewTextures(previewTextures);
            animationFrame_ = 0.0F;
            uploadedAnimationFrame_ = -1;
            refreshAnimatedMesh(true);
            refreshPreviewScene();
            lastAsset_ = "PMX " + model_->metadata.modelName + " — v"
                       + std::to_string(model_->metadata.version) + ", vertices "
                       + std::to_string(model_->metadata.vertexCount) + ", triangles "
                       + std::to_string(model_->indices.size() / 3) + ", bones "
                       + std::to_string(model_->bones.size()) + ", textures "
                       + std::to_string(loadedTextures) + "/" + std::to_string(model_->textures.size());
            log::info("Loaded metadata: ", lastAsset_, " (", path.string(), ")");
            projectAssets_.push_back({ "pmx", std::filesystem::absolute(path) });
        } catch (const std::exception& exception) {
            lastAsset_ = "PMX error: " + std::string(exception.what());
            log::warn(lastAsset_);
        }
        return;
    }
    if (kind == core::AssetKind::audio || kind == core::AssetKind::video) {
        try {
            animator_.reset();
            physics_.reset();
            model_.reset();
            motion_.reset();
            pose_.reset();
            media_ = std::make_unique<core::MediaFile>(path);
            mediaSeconds_ = 0.0;
            uploadedVideoFrame_ = -1;
            videoMode_ = media_->info().hasVideo;
            if (media_->info().hasAudio) audioPlayer_.play(media_->decodeAudio());
            if (videoMode_) {
                const std::array<graphics::PreviewVertex, 4> vertices {{
                    {{ -0.9F, -0.9F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 0.0F, 1.0F }},
                    {{  0.9F, -0.9F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 1.0F }},
                    {{  0.9F,  0.9F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F }},
                    {{ -0.9F,  0.9F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 0.0F, 0.0F }},
                }};
                const std::array<std::uint32_t, 6> indices { 0, 1, 2, 2, 3, 0 };
                device_->uploadPreviewMesh(vertices, indices);
                refreshVideoFrame();
            }
            lastAsset_ = std::string(core::toString(kind)) + " — "
                       + std::to_string(media_->info().durationSeconds) + " s";
            log::info("Loaded media: ", lastAsset_, " (", path.string(), ")");
            projectAssets_.push_back({ kind == core::AssetKind::audio ? "audio" : "video",
                                       std::filesystem::absolute(path) });
        } catch (const std::exception& exception) {
            lastAsset_ = "Media error: " + std::string(exception.what());
            log::warn(lastAsset_);
        }
        return;
    }
    if (kind == core::AssetKind::vmd) {
        try {
            motion_ = std::make_unique<core::VmdMotion>(core::loadVmd(path));
            manualCamera_ = false;
            if (animator_ != nullptr) { animator_->setMotion(motion_.get()); animationFrame_ = 0.0F; refreshAnimatedMesh(false); }
            refreshPreviewScene();
            lastAsset_ = "VMD " + motion_->modelName + " — " + std::to_string(motion_->bones.size())
                       + " bone keys, " + std::to_string(motion_->morphs.size()) + " morph keys, "
                       + std::to_string(motion_->lastFrame) + " frames";
            log::info("Loaded motion: ", lastAsset_, " (", path.string(), ")");
            projectAssets_.push_back({ "vmd", std::filesystem::absolute(path) });
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
            projectAssets_.push_back({ "vpd", std::filesystem::absolute(path) });
        } catch (const std::exception& exception) {
            lastAsset_ = "VPD error: " + std::string(exception.what());
            log::warn(lastAsset_);
        }
        return;
    }
    if (kind == core::AssetKind::effect) {
        try {
            effect_ = std::make_unique<core::EffectGraph>(core::loadEffectGraph(path));
            auto filename = path.filename().string();
            std::ranges::transform(filename, filename.begin(), [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            if (device_ != nullptr && filename.find("subayai") != std::string::npos) {
                device_->selectRenderer(graphics::RendererKind::subayai);
            } else if (device_ != nullptr && filename.find("bdpt") != std::string::npos) {
                device_->selectRenderer(graphics::RendererKind::bdpt);
            }
            lastAsset_ = "Effect " + path.filename().string() + " — "
                       + std::to_string(effect_->passes.size()) + " passes, "
                       + std::to_string(effect_->textures.size()) + " textures";
            log::info("Loaded effect graph: ", lastAsset_);
            projectAssets_.push_back({ "effect", std::filesystem::absolute(path) });
        } catch (const std::exception& exception) {
            lastAsset_ = "Effect error: " + std::string(exception.what());
            log::warn(lastAsset_);
        }
        return;
    }
    lastAsset_ = std::string(core::toString(kind)) + ": " + path.filename().string();
    log::info("Accepted ", core::toString(kind), " asset: ", path.string());
}

void Application::refreshAnimatedMesh(bool initialUpload, float deltaSeconds) {
    if (device_ == nullptr || model_ == nullptr || animator_ == nullptr) return;
    auto frame = animator_->evaluate(animationFrame_, deltaSeconds);
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
        material.textureSlot = model_->materials[i].textureIndex >= 0
            ? static_cast<std::uint32_t>(model_->materials[i].textureIndex + 1) : 0U;
        const auto& diffuse = i < frame.materialDiffuse.size() ? frame.materialDiffuse[i]
                                                               : model_->materials[i].diffuse;
        std::copy(diffuse.begin(), diffuse.end(), material.diffuse);
        materials.push_back(material);
        firstIndex += material.indexCount;
    }
    device_->updatePreviewMaterials(materials);
    uploadedAnimationFrame_ = static_cast<int>(animationFrame_);
}

void Application::refreshVideoFrame() {
    if (!videoMode_ || media_ == nullptr || device_ == nullptr) return;
    const auto image = media_->decodeVideoFrame(mediaSeconds_);
    const std::array textures { graphics::PreviewTexture { image.width, image.height, image.pixels } };
    device_->uploadPreviewTextures(textures);
    graphics::PreviewMaterial material;
    material.indexCount = 6;
    material.textureSlot = 1;
    const std::array materials { material };
    device_->updatePreviewMaterials(materials);
    uploadedVideoFrame_ = static_cast<std::int64_t>(mediaSeconds_ * media_->info().videoFramesPerSecond);
}

void Application::refreshPreviewScene() {
    if (device_ == nullptr) return;
    graphics::PreviewScene scene;
    scene.cameraRotation[0] = cameraPitch_;
    scene.cameraRotation[1] = cameraYaw_;
    scene.cameraDistance = cameraDistance_;
    if (!manualCamera_ && motion_ != nullptr && !motion_->cameras.empty()) {
        const auto camera = core::evaluateCamera(*motion_, animationFrame_);
        std::copy(camera.rotation.begin(), camera.rotation.end(), scene.cameraRotation);
        scene.cameraDistance = std::max(std::abs(camera.distance) * normalization_.scale, 0.4F);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            scene.target[axis] = (camera.position[axis] - normalization_.center[axis]) * normalization_.scale;
        }
        scene.verticalFovRadians = std::clamp(camera.viewAngle, 1.0F, 179.0F)
                                 * 0.01745329252F;
        scene.perspective = camera.perspective;
    }
    if (motion_ != nullptr && !motion_->lights.empty()) {
        const auto light = core::evaluateLight(*motion_, animationFrame_);
        std::copy(light.position.begin(), light.position.end(), scene.lightDirection);
    }
    device_->updatePreviewScene(scene);
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
        ImGui::TextUnformatted("Files: DAYO / PMX / VMD / VPD / images / audio / video / FXDAYO");
        ImGui::Text("OIDN: %s", DAYO_HAS_OIDN ? "CPU available; HIP optional" : "not installed");
        ImGui::Text("Media: %s", DAYO_HAS_MEDIA ? "FFmpeg enabled" : "metadata/drop only");
        if (motion_ != nullptr) {
            if (ImGui::Checkbox("Play", &playing_) && audioPlayer_.active()) audioPlayer_.setPaused(!playing_);
            float maximum = static_cast<float>(std::max(motion_->lastFrame, 1U));
            if (ImGui::SliderFloat("Frame", &animationFrame_, 0.0F, maximum, "%.1f")) {
                refreshAnimatedMesh(false);
                refreshPreviewScene();
            }
        }
        if (media_ != nullptr) {
            if (motion_ == nullptr && ImGui::Checkbox("Play", &playing_) && audioPlayer_.active()) {
                audioPlayer_.setPaused(!playing_);
            }
            ImGui::Text("Media: %.2f / %.2f s%s%s", mediaSeconds_, media_->info().durationSeconds,
                        media_->info().hasVideo ? " video" : "", media_->info().hasAudio ? " audio" : "");
        }
        if (physics_ != nullptr) {
            ImGui::Text("Bullet: %s (%zu bodies, %zu joints)", physics_->available() ? "enabled" : "unavailable",
                        physics_->bodyCount(), physics_->jointCount());
        }
        if (effect_ != nullptr) {
            ImGui::Text("Effect graph: %zu passes / %zu textures / %zu samplers",
                        effect_->passes.size(), effect_->textures.size(), effect_->samplers.size());
        }
        ImGui::TextUnformatted("Camera: right-drag to orbit, wheel to zoom");
        if (manualCamera_ && ImGui::Button("Use VMD camera")) {
            manualCamera_ = false;
            refreshPreviewScene();
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Subayai and BDPT are enabled only when Vulkan RT features are present.");
    }
    ImGui::End();
#endif
}

} // namespace dayo::app
