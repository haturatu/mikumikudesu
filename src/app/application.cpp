#include "app/application.hpp"

#include "core/asset.hpp"
#include "core/animation.hpp"
#include "core/denoiser.hpp"
#include "core/image.hpp"
#include "core/log.hpp"
#include "core/model_probe.hpp"
#include "core/motion.hpp"
#include "core/vmdayo.hpp"
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
        if (scene_.advanceFrame(deltaSeconds, playing_)) {
            animationFrame_ = scene_.timeline().frame;
            const int integerFrame = static_cast<int>(animationFrame_);
            if (integerFrame != uploadedAnimationFrame_ && !scene_.models().empty()) {
                refreshAnimatedMesh(false, deltaSeconds);
            }
            refreshPreviewScene();
        }
        auto* media = scene_.media();
        if (effectReloader_) {
            std::string reloadError;
            if (effectReloader_->poll(&reloadError) && effectReloader_->current() != nullptr) {
                scene_.setEffect(*effectReloader_->current());
                log::info("Hot reloaded effect graph");
            } else if (!reloadError.empty()) {
                log::warn("FX hot reload deferred: ", reloadError);
            }
        }
        if (playing_ && videoMode_ && media != nullptr && media->info().hasVideo) {
            mediaSeconds_ += deltaSeconds;
            if (media->info().durationSeconds > 0.0 && mediaSeconds_ >= media->info().durationSeconds) {
                mediaSeconds_ = std::fmod(mediaSeconds_, media->info().durationSeconds);
                uploadedVideoFrame_ = -1;
                if (media->info().hasAudio) audioPlayer_.play(media->decodeAudio());
            }
            const auto videoFrame = static_cast<std::int64_t>(mediaSeconds_ * media->info().videoFramesPerSecond);
            if (videoFrame != uploadedVideoFrame_) refreshVideoFrame();
        }
        device->beginUiFrame();
        buildUi();
        device->renderFrame();
        if (scene_.runtimeMode() == core::RuntimeMode::accumulate) scene_.advanceAccumulation();
        else if (scene_.runtimeMode() == core::RuntimeMode::realtime) scene_.invalidateAccumulation();
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
            scene_.clearModels();
            scene_.clearBackground();
            scene_.clearMedia();
            scene_.clearEffect();
            projectAssets_.clear();
            if (project.renderer == "subayai") device_->selectRenderer(graphics::RendererKind::subayai);
            else if (project.renderer == "bdpt") device_->selectRenderer(graphics::RendererKind::bdpt);
            else device_->selectRenderer(graphics::RendererKind::preview);
            for (const auto& asset : project.assets) handleAsset(asset.path);
            if (project.embeddedMotion && selectedModel() != nullptr) {
                auto* target = selectedModel();
                target->motion = std::make_unique<core::VmdMotion>(*project.embeddedMotion);
                target->animator->setMotion(target->motion.get());
                manualCamera_ = false;
            }
            animationFrame_ = project.frame;
            scene_.setFrame(animationFrame_);
            playing_ = project.playing;
            if (!scene_.models().empty()) refreshAnimatedMesh(false);
            lastAsset_ = "Project " + path.filename().string() + " — "
                       + std::to_string(project.assets.size()) + " assets";
            log::info("Loaded project: ", lastAsset_);
        } catch (const std::exception& exception) {
            lastAsset_ = "Project error: " + std::string(exception.what());
            log::warn(lastAsset_);
        }
        return;
    }
    if (kind == core::AssetKind::vmdayo) {
        try {
            const auto document = core::loadVmdayo(path);
            if (selectedModel() == nullptr) throw std::runtime_error("VMdayo requires a selected model");
            auto motion = std::make_unique<core::VmdMotion>();
            motion->modelName = document.modelName;
            motion->bones = document.motion.bones;
            motion->morphs = document.motion.morphs;
            motion->cameras = document.motion.cameras;
            motion->lights = document.motion.lights;
            motion->shadows = document.motion.shadows;
            motion->ik = document.motion.ik;
            motion->interpolation = document.motion.interpolation;
            for (const auto& key : motion->bones) motion->lastFrame = std::max(motion->lastFrame, key.frame);
            for (const auto& key : motion->morphs) motion->lastFrame = std::max(motion->lastFrame, key.frame);
            for (const auto& key : motion->cameras) motion->lastFrame = std::max(motion->lastFrame, key.frame);
            auto* model = selectedModel();
            model->motion = std::move(motion);
            model->animator->setMotion(model->motion.get());
            animationFrame_ = 0.0F;
            scene_.setFrame(animationFrame_);
            refreshAnimatedMesh(false);
            refreshPreviewScene();
            lastAsset_ = "VMdayo " + path.filename().string();
            projectAssets_.push_back({ "vmdayo", std::filesystem::absolute(path) });
            log::info("Loaded VMdayo motion: ", path.string());
        } catch (const std::exception& exception) {
            lastAsset_ = "VMdayo error: " + std::string(exception.what());
            log::warn(lastAsset_);
        }
        return;
    }
    if (kind == core::AssetKind::image) {
        try {
            auto image = core::loadImageRgba8(path);
            scene_.setBackgroundImage(path);
            videoMode_ = false;
            const std::array<graphics::PreviewVertex, 4> vertices {{
                {{ -1.0F, -1.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 0.0F, 1.0F }},
                {{  1.0F, -1.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 1.0F }},
                {{  1.0F,  1.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F }},
                {{ -1.0F,  1.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 0.0F, 0.0F }},
            }};
            const std::array<std::uint32_t, 6> indices { 0, 1, 2, 2, 3, 0 };
            if (scene_.models().empty()) {
                device_->uploadPreviewMesh(vertices, indices);
                const std::array previewTextures { graphics::PreviewTexture {
                    image.width, image.height, image.pixels } };
                device_->uploadPreviewTextures(previewTextures);
                graphics::PreviewMaterial material;
                material.indexCount = 6;
                material.textureSlot = 1;
                const std::array materials { material };
                device_->updatePreviewMaterials(materials);
                graphics::PreviewScene preview;
                preview.cameraDistance = 2.42F;
                device_->updatePreviewScene(preview);
            } else {
                refreshPreviewTextures();
                refreshAnimatedMesh(true);
                refreshPreviewScene();
            }
            lastAsset_ = "Image " + path.filename().string() + " — "
                       + std::to_string(image.width) + "x" + std::to_string(image.height);
            projectAssets_.push_back({ "image", std::filesystem::absolute(path) });
            log::info("Loaded image: ", lastAsset_);
        } catch (const std::exception& exception) {
            lastAsset_ = "Image error: " + std::string(exception.what());
            log::warn(lastAsset_);
        }
        return;
    }
    if (kind == core::AssetKind::pmx) {
        try {
            const auto modelId = scene_.addModel(path);
            scene_.selectModel(modelId);
            videoMode_ = scene_.media() != nullptr && scene_.media()->info().hasVideo;
            normalization_ = scene_.selectedModel()->normalization;
            refreshPreviewTextures();
            animationFrame_ = 0.0F;
            scene_.setFrame(animationFrame_);
            uploadedAnimationFrame_ = -1;
            refreshAnimatedMesh(true);
            refreshPreviewScene();
            const auto* model = selectedModel();
            lastAsset_ = "PMX " + model->model->metadata.modelName + " — v"
                       + std::to_string(model->model->metadata.version) + ", vertices "
                       + std::to_string(model->model->metadata.vertexCount) + ", triangles "
                       + std::to_string(model->model->indices.size() / 3) + ", bones "
                       + std::to_string(model->model->bones.size()) + ", models "
                       + std::to_string(scene_.models().size());
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
            scene_.setMedia(path);
            auto* media = scene_.media();
            mediaSeconds_ = 0.0;
            uploadedVideoFrame_ = -1;
            videoMode_ = media->info().hasVideo;
            if (media->info().hasAudio) audioPlayer_.play(media->decodeAudio());
            if (videoMode_ && scene_.models().empty()) {
                const std::array<graphics::PreviewVertex, 4> vertices {{
                    {{ -0.9F, -0.9F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 0.0F, 1.0F }},
                    {{  0.9F, -0.9F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 1.0F }},
                    {{  0.9F,  0.9F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F }},
                    {{ -0.9F,  0.9F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 0.0F, 0.0F }},
                }};
                const std::array<std::uint32_t, 6> indices { 0, 1, 2, 2, 3, 0 };
                device_->uploadPreviewMesh(vertices, indices);
                refreshVideoFrame();
            } else if (!scene_.models().empty()) {
                refreshPreviewTextures();
                refreshAnimatedMesh(true);
            }
            lastAsset_ = std::string(core::toString(kind)) + " — "
                       + std::to_string(media->info().durationSeconds) + " s";
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
            scene_.attachMotion(path);
            manualCamera_ = false;
            animationFrame_ = 0.0F;
            scene_.setFrame(animationFrame_);
            if (selectedModel() != nullptr) refreshAnimatedMesh(false);
            refreshPreviewScene();
            const auto* model = selectedModel();
            const auto* motion = model != nullptr ? model->motion.get() : nullptr;
            lastAsset_ = "VMD " + (motion != nullptr ? motion->modelName : path.filename().string())
                       + " — " + std::to_string(motion != nullptr ? motion->bones.size() : 0) + " bone keys, "
                       + std::to_string(motion != nullptr ? motion->morphs.size() : 0) + " morph keys, "
                       + std::to_string(motion != nullptr ? motion->lastFrame : 0) + " frames";
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
            scene_.attachPose(path);
            if (selectedModel() != nullptr) refreshAnimatedMesh(false);
            const auto* pose = selectedModel()->pose.get();
            lastAsset_ = "VPD pose — " + std::to_string(pose->bones.size()) + " bones";
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
            effectReloader_.emplace(path);
            static_cast<void>(effectReloader_->poll());
            if (effectReloader_->current() == nullptr) throw std::runtime_error("effect graph is empty");
            scene_.setEffect(*effectReloader_->current());
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
                       + std::to_string(scene_.effect()->passes.size()) + " passes, "
                       + std::to_string(scene_.effect()->textures.size()) + " textures";
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
    if (device_ == nullptr || scene_.models().empty()) return;
    std::vector<graphics::PreviewVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<graphics::PreviewMaterial> materials;
    for (const auto& instance : scene_.models()) {
        if (!instance.visible || instance.model == nullptr || instance.animator == nullptr) continue;
        const auto gravity = scene_.evaluatePhysicsSettings(animationFrame_);
        if (instance.physics != nullptr) {
            instance.physics->setGravity({ gravity.gravityDirection[0] * gravity.gravity,
                                           gravity.gravityDirection[1] * gravity.gravity,
                                           gravity.gravityDirection[2] * gravity.gravity });
            instance.physics->setGravityNoise(gravity.noiseAmplitude, gravity.noiseFrequency);
            instance.physics->setFloorCollision(gravity.floorCollision);
        }
        const auto frame = instance.animator->evaluate(animationFrame_, deltaSeconds);
        auto normalizedFrame = frame;
        if (instance.softBody != nullptr && instance.softBody->available()) {
            instance.softBody->step(deltaSeconds, { gravity.gravityDirection[0] * gravity.gravity,
                                                    gravity.gravityDirection[1] * gravity.gravity,
                                                    gravity.gravityDirection[2] * gravity.gravity });
            instance.softBody->apply(normalizedFrame.vertices);
        }
        core::normalizeForPreview(normalizedFrame.vertices, *instance.model);
        const auto textureBase = [&] {
            std::size_t value = 0;
            for (const auto& previous : scene_.models()) {
                if (previous.id == instance.id) break;
                value += previous.textures.size();
            }
            return static_cast<std::uint32_t>(value);
        }();
        const auto cloneCount = std::max(instance.cloneCount, 1U);
        for (std::uint32_t clone = 0; clone < cloneCount; ++clone) {
            const auto baseVertex = static_cast<std::uint32_t>(vertices.size());
            const float cloneOffset = (static_cast<float>(clone)
                                     - static_cast<float>(cloneCount - 1U) * 0.5F) * 2.2F;
            for (const auto& source : normalizedFrame.vertices) {
                graphics::PreviewVertex vertex;
                std::memcpy(vertex.position, source.position.data(), sizeof(vertex.position));
                vertex.position[0] += cloneOffset;
                std::memcpy(vertex.normal, source.normal.data(), sizeof(vertex.normal));
                std::memcpy(vertex.uv, source.uv.data(), sizeof(vertex.uv));
                vertices.push_back(vertex);
            }
            for (const auto index : instance.model->indices) indices.push_back(baseVertex + index);
            std::uint32_t firstIndex = static_cast<std::uint32_t>(indices.size() - instance.model->indices.size());
            for (std::size_t materialIndex = 0; materialIndex < instance.model->materials.size(); ++materialIndex) {
                graphics::PreviewMaterial material;
                material.firstIndex = firstIndex;
                material.indexCount = instance.model->materials[materialIndex].indexCount;
                material.doubleSided = (instance.model->materials[materialIndex].drawFlags & 0x01U) != 0;
                material.textureSlot = instance.model->materials[materialIndex].textureIndex >= 0
                    ? textureBase + static_cast<std::uint32_t>(instance.model->materials[materialIndex].textureIndex) + 1U : 0U;
                if (materialIndex < normalizedFrame.materials.size()) {
                    const auto& animated = normalizedFrame.materials[materialIndex];
                    std::copy(animated.diffuse.begin(), animated.diffuse.end(), material.diffuse);
                    std::copy(animated.ambient.begin(), animated.ambient.end(), material.ambient);
                    std::copy(animated.specular.begin(), animated.specular.end(), material.specular);
                    material.shininess = animated.shininess;
                    std::copy(animated.textureMultiply.begin(), animated.textureMultiply.end(), material.textureMultiply);
                    std::copy(animated.textureAdd.begin(), animated.textureAdd.end(), material.textureAdd);
                }
                materials.push_back(material);
                firstIndex += material.indexCount;
            }
        }
    }
    if (vertices.empty() || indices.empty()) return;
    if (initialUpload) device_->uploadPreviewMesh(vertices, indices);
    else {
        try { device_->updatePreviewVertices(vertices); }
        catch (const std::exception&) { device_->uploadPreviewMesh(vertices, indices); }
    }
    device_->updatePreviewMaterials(materials);
    uploadedAnimationFrame_ = static_cast<int>(animationFrame_);
    scene_.clearDirty(core::DirtyFlag::geometry | core::DirtyFlag::material);
}

void Application::refreshPreviewTextures() {
    if (device_ == nullptr) return;
    textures_.clear();
    for (const auto& instance : scene_.models()) {
        textures_.insert(textures_.end(), instance.textures.begin(), instance.textures.end());
    }
    std::vector<graphics::PreviewTexture> previewTextures;
    previewTextures.reserve(textures_.size());
    for (const auto& texture : textures_) {
        previewTextures.push_back({ texture.width, texture.height, texture.pixels });
    }
    device_->uploadPreviewTextures(previewTextures);
}

void Application::refreshVideoFrame() {
    auto* media = scene_.media();
    if (!videoMode_ || media == nullptr || device_ == nullptr) return;
    const auto image = media->decodeVideoFrame(mediaSeconds_);
    if (!scene_.models().empty()) {
        // With a model present, the frame is retained as the reserved
        // background source and must not replace the model texture array.
        scene_.setBackgroundScreenSource(core::ScreenTextureSource::backgroundVideo);
        uploadedVideoFrame_ = static_cast<std::int64_t>(mediaSeconds_ * media->info().videoFramesPerSecond);
        return;
    }
    const std::array textures { graphics::PreviewTexture { image.width, image.height, image.pixels } };
    device_->uploadPreviewTextures(textures);
    graphics::PreviewMaterial material;
    material.indexCount = 6;
    material.textureSlot = 1;
    const std::array materials { material };
    device_->updatePreviewMaterials(materials);
    uploadedVideoFrame_ = static_cast<std::int64_t>(mediaSeconds_ * media->info().videoFramesPerSecond);
}

void Application::refreshPreviewScene() {
    if (device_ == nullptr) return;
    const auto* model = selectedModel();
    const auto* motion = scene_.cameraMotion() != nullptr ? scene_.cameraMotion()
        : (model != nullptr ? model->motion.get() : nullptr);
    graphics::PreviewScene scene;
    switch (scene_.background().screenSource) {
    case core::ScreenTextureSource::previousFrame:
        scene.screenSource = graphics::PreviewScene::ScreenSource::previousFrame; break;
    case core::ScreenTextureSource::backgroundVideo:
        scene.screenSource = graphics::PreviewScene::ScreenSource::backgroundVideo; break;
    case core::ScreenTextureSource::backgroundImage:
        scene.screenSource = graphics::PreviewScene::ScreenSource::backgroundImage; break;
    case core::ScreenTextureSource::white:
        scene.screenSource = graphics::PreviewScene::ScreenSource::white; break;
    }
    scene.screenCrop = scene_.background().crop == core::ScreenCropMode::crop4x3
        ? graphics::PreviewScene::ScreenCrop::crop4x3 : graphics::PreviewScene::ScreenCrop::none;
    scene.backgroundEnabled = scene_.background().enabled;
    scene.cameraRotation[0] = cameraPitch_;
    scene.cameraRotation[1] = cameraYaw_;
    scene.cameraDistance = cameraDistance_;
    if (!manualCamera_ && motion != nullptr && !motion->cameras.empty()) {
        const auto camera = core::evaluateCamera(*motion, animationFrame_);
        std::copy(camera.rotation.begin(), camera.rotation.end(), scene.cameraRotation);
        const auto normalization = model != nullptr ? model->normalization : normalization_;
        scene.cameraDistance = std::max(std::abs(camera.distance) * normalization.scale, 0.4F);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            scene.target[axis] = (camera.position[axis] - normalization.center[axis]) * normalization.scale;
        }
        scene.verticalFovRadians = std::clamp(camera.viewAngle, 1.0F, 179.0F)
                                 * 0.01745329252F;
        scene.perspective = camera.perspective;
    }
    if (motion != nullptr && !motion->lights.empty()) {
        const auto light = core::evaluateLight(*motion, animationFrame_);
        std::copy(light.position.begin(), light.position.end(), scene.lightDirection);
    }
    device_->updatePreviewScene(scene);
}

void Application::buildUi() {
#if DAYO_HAS_IMGUI
    ImGui::SetNextWindowPos({ 24.0F, 24.0F }, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({ 460.0F, 420.0F }, ImGuiCond_FirstUseEver);
    auto* model = selectedModel();
    const auto* motion = scene_.cameraMotion() != nullptr ? scene_.cameraMotion()
        : (model != nullptr ? model->motion.get() : nullptr);
    auto* media = scene_.media();
    if (ImGui::Begin("Renderer")) {
        ImGui::TextUnformatted("SDL3 + Vulkan native backend");
        ImGui::Separator();
        ImGui::Text("Renderer request: %s", graphics::toString(options_.renderer).data());
        ImGui::TextWrapped("%s", lastAsset_.c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted("Files: DAYO / PMX / VMD / VPD / images / audio / video / FXDAYO");
        ImGui::Text("OIDN: %s", DAYO_HAS_OIDN ? "CPU available; HIP optional" : "not installed");
        ImGui::Text("Media: %s", DAYO_HAS_MEDIA ? "FFmpeg enabled" : "metadata/drop only");
        if (motion != nullptr) {
            if (ImGui::Checkbox("Play", &playing_) && audioPlayer_.active()) audioPlayer_.setPaused(!playing_);
            float maximum = std::max(scene_.timeline().duration, 1.0F);
            if (ImGui::SliderFloat("Frame", &animationFrame_, 0.0F, maximum, "%.1f")) {
                const auto before = scene_.timeline().frame;
                history_.execute(scene_, std::make_unique<core::SetFrameCommand>(before, animationFrame_));
                refreshAnimatedMesh(false);
                refreshPreviewScene();
            }
        }
        if (media != nullptr) {
            if (motion == nullptr && ImGui::Checkbox("Play", &playing_) && audioPlayer_.active()) {
                audioPlayer_.setPaused(!playing_);
            }
            ImGui::Text("Media: %.2f / %.2f s%s%s", mediaSeconds_, media->info().durationSeconds,
                        media->info().hasVideo ? " video" : "", media->info().hasAudio ? " audio" : "");
        }
        if (model != nullptr && model->physics != nullptr) {
            ImGui::Text("Bullet: %s (%zu bodies, %zu joints)", model->physics->available() ? "enabled" : "unavailable",
                        model->physics->bodyCount(), model->physics->jointCount());
            if (model->softBody != nullptr) {
                ImGui::Text("Soft body: %s (%zu)", model->softBody->available() ? "fallback" : "none",
                            model->softBody->bodyCount());
            }
        }
        if (scene_.effect() != nullptr) {
            ImGui::Text("Effect graph: %zu passes / %zu textures / %zu samplers",
                        scene_.effect()->passes.size(), scene_.effect()->textures.size(), scene_.effect()->samplers.size());
        }
        ImGui::TextUnformatted("Camera: right-drag to orbit, wheel to zoom");
        if (ImGui::Button("Undo")) {
            if (history_.undo(scene_)) {
                animationFrame_ = scene_.timeline().frame;
                refreshAnimatedMesh(false);
                refreshPreviewScene();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Redo")) {
            if (history_.redo(scene_)) {
                animationFrame_ = scene_.timeline().frame;
                refreshAnimatedMesh(false);
                refreshPreviewScene();
            }
        }
        if (manualCamera_ && ImGui::Button("Use VMD camera")) {
            manualCamera_ = false;
            refreshPreviewScene();
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Subayai and BDPT are enabled only when Vulkan RT features are present.");
    }
    ImGui::End();

    if (ImGui::Begin("Models")) {
        ImGui::Text("%zu model instance(s)", scene_.models().size());
        for (const auto& instance : scene_.models()) {
            ImGui::PushID(static_cast<int>(instance.id));
            bool selected = scene_.selectedModelId() == instance.id;
            if (ImGui::Selectable(instance.displayName.c_str(), selected)) {
                scene_.selectModel(instance.id);
                normalization_ = instance.normalization;
                refreshAnimatedMesh(true);
                refreshPreviewScene();
            }
            ImGui::SameLine();
            bool visible = instance.visible;
            if (ImGui::Checkbox("Visible", &visible)) {
                scene_.setModelVisible(instance.id, visible);
                refreshAnimatedMesh(true);
            }
            ImGui::PopID();
        }
        if (model != nullptr) {
            int clones = static_cast<int>(model->cloneCount);
            if (ImGui::SliderInt("Clone count", &clones, 1, 16)) {
                scene_.setCloneCount(model->id, static_cast<std::uint32_t>(clones));
                refreshAnimatedMesh(true);
            }
        }
    }
    ImGui::End();

    if (ImGui::Begin("Animation / Physics")) {
        ImGui::Text("Timeline: %.1f / %.1f frames", animationFrame_, scene_.timeline().duration);
        auto settings = scene_.physicsSettings();
        bool physicsChanged = ImGui::DragFloat("Gravity", &settings.gravity, 0.01F, 0.0F, 100.0F);
        physicsChanged |= ImGui::DragFloat3("Gravity direction", settings.gravityDirection.data(), 0.01F);
        physicsChanged |= ImGui::DragFloat("Gravity noise amplitude", &settings.noiseAmplitude, 0.01F, 0.0F, 100.0F);
        physicsChanged |= ImGui::DragFloat("Gravity noise frequency", &settings.noiseFrequency, 0.01F, 0.0F, 100.0F);
        physicsChanged |= ImGui::Checkbox("Floor collision", &settings.floorCollision);
        if (physicsChanged) scene_.setPhysicsSettings(settings);
        int runtimeMode = static_cast<int>(scene_.runtimeMode());
        if (ImGui::Combo("Runtime mode", &runtimeMode, "Accumulate\0Realtime\0Idle\0")) {
            history_.execute(scene_, std::make_unique<core::SetRuntimeModeCommand>(
                scene_.runtimeMode(), static_cast<core::RuntimeMode>(runtimeMode)));
        }
        ImGui::Text("Accumulated samples: %llu", static_cast<unsigned long long>(scene_.accumulatedSamples()));
        if (ImGui::Button("Reset physics") && model != nullptr && model->physics != nullptr) model->physics->reset();
    }
    ImGui::End();
#endif
}

} // namespace dayo::app
