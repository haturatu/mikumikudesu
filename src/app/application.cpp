#include "app/application.hpp"

#include "core/animation.hpp"
#include "core/asset.hpp"
#include "core/denoiser.hpp"
#include "core/image.hpp"
#include "core/log.hpp"
#include "core/model_probe.hpp"
#include "core/motion.hpp"
#include "core/video_export.hpp"
#include "core/vmdayo.hpp"
#include "graphics/device.hpp"
#include "platform/window.hpp"
#include "ui/theme.hpp"

#include <SDL3/SDL.h>

#if DAYO_HAS_IMGUI
#include <imgui.h>
#include <imgui_internal.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace dayo::app {
namespace {

double sceneTimelineFps(const core::Scene& scene) noexcept {
    const auto value = static_cast<double>(scene.timeline().fps);
    return std::isfinite(value) && value > 0.0 ? value : 30.0;
}

core::Float3 rotateQuaternion(const core::Float4& quaternion, const core::Float3& value) {
    const core::Float3 axis{quaternion[0], quaternion[1], quaternion[2]};
    const core::Float3 firstCross{
        axis[1] * value[2] - axis[2] * value[1],
        axis[2] * value[0] - axis[0] * value[2],
        axis[0] * value[1] - axis[1] * value[0],
    };
    const core::Float3 nested{
        axis[1] * firstCross[2] - axis[2] * firstCross[1],
        axis[2] * firstCross[0] - axis[0] * firstCross[2],
        axis[0] * firstCross[1] - axis[1] * firstCross[0],
    };
    return {
        value[0] + 2.0F * (nested[0] + quaternion[3] * firstCross[0]),
        value[1] + 2.0F * (nested[1] + quaternion[3] * firstCross[1]),
        value[2] + 2.0F * (nested[2] + quaternion[3] * firstCross[2]),
    };
}

bool hasTransparentPixels(const core::ImageRgba8& image) {
    if (image.pixels.size() < 4)
        return false;
    for (std::size_t index = 3; index < image.pixels.size(); index += 4) {
        if (image.pixels[index] < 250U)
            return true;
    }
    return false;
}

bool hasLoadedTexture(const std::vector<core::ImageRgba8>& textures, std::int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= textures.size())
        return false;
    const auto& texture = textures[static_cast<std::size_t>(index)];
    return texture.width != 0 && texture.height != 0 && !texture.pixels.empty();
}

core::Float3 normalizePreviewPoint(const core::Float3& point, const core::PreviewNormalization& normalization) {
    return {
        (point[0] - normalization.center[0]) * normalization.scale,
        (point[1] - normalization.center[1]) * normalization.scale,
        (point[2] - normalization.center[2]) * normalization.scale,
    };
}

std::uint64_t videoOutputFrameCount(std::uint64_t firstFrame, std::uint64_t lastFrame, double sourceFps,
                                    double outputFps) {
    const auto intervals = static_cast<double>(lastFrame - firstFrame) * outputFps / sourceFps;
    if (!std::isfinite(intervals) || intervals > static_cast<double>(std::numeric_limits<std::uint64_t>::max() - 1U)) {
        throw std::invalid_argument("video frame range produces too many output frames");
    }
    return static_cast<std::uint64_t>(std::ceil(std::max(intervals, 0.0) - 1e-9)) + 1U;
}

float videoSourceFrame(std::uint64_t outputFrame, std::uint64_t firstFrame, std::uint64_t lastFrame, double sourceFps,
                       double outputFps) noexcept {
    const auto value = static_cast<double>(firstFrame) + static_cast<double>(outputFrame) * sourceFps / outputFps;
    return static_cast<float>(std::min(static_cast<double>(lastFrame), value));
}

#if DAYO_HAS_IMGUI
const char* workspaceSuffix(ui::Workspace workspace) noexcept {
    switch (workspace) {
    case ui::Workspace::layout:
        return "Layout";
    case ui::Workspace::animation:
        return "Animation";
    case ui::Workspace::camera:
        return "Camera";
    case ui::Workspace::render:
        return "Render";
    case ui::Workspace::debug:
        return "Debug";
    }
    return "Layout";
}
#endif

} // namespace

Application::Application(Options options) : options_(std::move(options)) {}

std::string Application::workspaceWindowName(const char* title, const char* id) const {
#if DAYO_HAS_IMGUI
    return std::string(title) + "##" + id + "." + workspaceSuffix(uiState_.workspace);
#else
    return std::string(title) + "##" + id;
#endif
}

void Application::resetProjectRuntimeState() {
#if DAYO_HAS_IMGUI
    if (imageSequenceExportRunning_) {
        if (imageSequenceOutput_)
            imageSequenceOutput_->close();
        imageSequenceOutput_.reset();
        imageSequenceExportRunning_ = false;
        imageSequenceRestoring_ = false;
    }
    timelineTrackCache_ = {};
#endif
    videoExportJob_.cancel();
    videoExportUiActive_ = false;
    videoExportFramesFinished_ = false;
    videoExportRestorePending_ = false;
    activeVideoExport_.reset();
    videoRangeInitialized_ = false;
    scene_.clearProjectState();
    if (device_ != nullptr)
        device_->clearPreviewResources();
    effectReloader_.reset();
    audioPlayer_.stop();
    audioSource_.clear();
    audioDestination_.fill('\0');
    audioFromSeconds_ = 0.0F;
    audioToSeconds_ = 0.0F;
    textures_.clear();
    animatedIndices_.clear();
    animatedMorphDeltas_.clear();
    animatedMorphRanges_.clear();
    animatedVertexCount_ = 0;
    animatedMaterialTemplates_.clear();
    animatedTopologyGeneration_ = 0;
    mediaSeconds_ = 0.0;
    uploadedVideoFrame_ = -1;
    videoMode_ = false;
    animationFrame_ = 0.0F;
    uploadedAnimationFrame_ = -1;
    playing_ = true;
    manualCamera_ = false;
    cameraYaw_ = 0.0F;
    cameraPitch_ = 0.0F;
    cameraDistance_ = 3.0F;
    normalization_ = {};
    projectAssets_.clear();
    history_.clear();
    frameProfiler_.reset();
}

int Application::run() {
    if (options_.audioExport && options_.videoExport) {
        throw std::invalid_argument("choose either --export-m4a or --export-video");
    }
    platform::WindowOptions windowOptions;
    windowOptions.title = "mikumikudesu — SDL3 + Vulkan";
    windowOptions.hidden = options_.hidden || options_.probeOnly || options_.videoExport.has_value();
    auto window = platform::createWindow(windowOptions);
    auto device = graphics::createVulkanDevice(*window, options_.validation);
    device_ = device.get();
    device->selectRenderer(options_.renderer);
    log::info("Graphics convention: depth [0,1], Vulkan framebuffer Y handled in backend");
    const auto denoiser = core::selectDenoiser();
    log::info("Denoiser: ", denoiser.detail);
    log::info("Media: ", DAYO_HAS_MEDIA ? "FFmpeg available" : "FFmpeg development libraries not found");

    for (const auto& asset : options_.assets)
        handleAsset(asset);
    if (options_.saveProject) {
        core::saveProject(*options_.saveProject, currentProject());
        log::info("Saved project: ", options_.saveProject->string());
    }
    if (options_.probeOnly) {
        std::cout << device->capabilities().json() << '\n';
        return 0;
    }
    if (options_.videoExport)
        return runVideoExport();

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
            case platform::WindowEvent::Type::displayScaleChanged:
#if DAYO_HAS_IMGUI
                uiState_.userScale = std::max(event.x, 1.0F);
                ui::applyEditorTheme(uiState_.userScale);
                ImGui::GetStyle().FontScaleDpi = uiState_.userScale;
#endif
                break;
            case platform::WindowEvent::Type::fileDropped:
                handleAsset(event.path);
                break;
            case platform::WindowEvent::Type::cameraDragged:
#if DAYO_HAS_IMGUI
                // Camera input is consumed from ImGui while drawing the viewport so
                // it cannot leak from another docked panel or use stale hover state.
            case platform::WindowEvent::Type::cameraZoomed:
                // See cameraDragged: read the current ImGui mouse state in the viewport.
                break;
#else
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
#endif
            }
        }
        if (!running)
            break;
        if (window->minimized() || window->pixelWidth() == 0 || window->pixelHeight() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        const auto tick = std::chrono::steady_clock::now();
        const float deltaSeconds = std::chrono::duration<float>(tick - previousTick).count();
        previousTick = tick;
        frameProfiler_.beginFrame();
#if DAYO_HAS_IMGUI
        if (imageSequenceExportRunning_) {
            advanceImageSequenceExport();
        } else if (videoExportUiActive_) {
#else
        if (videoExportUiActive_) {
#endif
            if (videoExportFramesFinished_) {
                if (!videoExportJob_.running()) {
                    restoreVideoExportState();
                    videoExportUiActive_ = videoExportRestorePending_;
                }
            } else if (videoExportJob_.running()) {
                const auto& exportOptions = activeVideoExport_.value();
                if (videoNextFrame_ < videoOutputFrameCount_ && videoExportJob_.canAcceptFrame()) {
                    if (!videoPreRollDone_) {
                        videoPreRollDone_ = advanceDeterministicFrameEvaluation(static_cast<float>(videoFromFrame_),
                                                                                videoEvaluationNextFrame_);
                        videoPreviousSourceFrame_ = static_cast<float>(videoFromFrame_);
                    }
                    if (videoPreRollDone_) {
                        const auto sourceFrame = videoSourceFrame(videoNextFrame_, videoFromFrame_, videoToFrame_,
                                                                  videoSourceFps_, videoFps_);
                        if (videoNextFrame_ != 0U) {
                            animationFrame_ = sourceFrame;
                            scene_.setFrame(animationFrame_);
                            const auto delta = std::max(0.0F, sourceFrame - videoPreviousSourceFrame_) /
                                               static_cast<float>(videoSourceFps_);
                            refreshAnimatedMesh(false, delta);
                        }
                        if (videoMode_) {
                            mediaSeconds_ = std::max(0.0, static_cast<double>(sourceFrame) / videoSourceFps_);
                            refreshVideoFrame();
                        }
                        refreshPreviewScene();
                        try {
                            if (videoExportJob_.trySubmitFrame(
                                    device_->renderToImage({exportOptions.width, exportOptions.height})))
                                ++videoNextFrame_;
                        } catch (const std::exception& exception) {
                            videoExportStatus_ = exception.what();
                            videoExportJob_.requestCancel();
                            videoExportFramesFinished_ = true;
                        }
                        videoPreviousSourceFrame_ = sourceFrame;
                    }
                } else if (videoNextFrame_ >= videoOutputFrameCount_) {
                    videoExportJob_.finishFrames();
                    videoExportFramesFinished_ = true;
                }
            } else if (!videoExportFramesFinished_ && !videoExportJob_.running()) {
                const auto error = videoExportJob_.error();
                videoExportStatus_ = error ? *error : "Video export stopped unexpectedly";
                videoExportFramesFinished_ = true;
                restoreVideoExportState();
                videoExportUiActive_ = videoExportRestorePending_;
            }
        } else if (scene_.advanceFrame(deltaSeconds * playbackSpeed_, playing_)) {
            animationFrame_ = scene_.timeline().frame;
            const int integerFrame = static_cast<int>(animationFrame_);
            if (integerFrame != uploadedAnimationFrame_ && !scene_.models().empty()) {
                refreshAnimatedMesh(false, deltaSeconds * playbackSpeed_);
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
            mediaSeconds_ += deltaSeconds * static_cast<double>(playbackSpeed_);
            if (media->info().durationSeconds > 0.0 && mediaSeconds_ >= media->info().durationSeconds) {
                if (repeat_)
                    mediaSeconds_ = std::fmod(mediaSeconds_, media->info().durationSeconds);
                else {
                    mediaSeconds_ = media->info().durationSeconds;
                    playing_ = false;
                    audioPlayer_.setPaused(true);
                }
                uploadedVideoFrame_ = -1;
                if (repeat_ && media->info().hasAudio) {
                    if (loadedAudio_.samples.empty())
                        loadedAudio_ = media->decodeAudio();
                    audioPlayer_.play(loadedAudio_, std::max(0.0F, audioOffsetSeconds_));
                    audioPlayer_.setVolume(audioVolume_);
                }
            }
            const auto videoFrame = static_cast<std::int64_t>(mediaSeconds_ * media->info().videoFramesPerSecond);
            if (videoFrame != uploadedVideoFrame_)
                refreshVideoFrame();
        }
        device->beginUiFrame();
        buildUi();
        frameProfiler_.addDrawStats(animatedVertexCount_, static_cast<std::uint64_t>(animatedDraws_.size()));
        {
            auto render = frameProfiler_.measure(core::ProfileSection::render);
            device->renderFrame();
            frameProfiler_.setGpuNanoseconds(device->previewGpuNanoseconds());
            render.finish();
        }
        frameProfiler_.endFrame();
        if (scene_.runtimeMode() == core::RuntimeMode::accumulate)
            scene_.advanceAccumulation();
        else if (scene_.runtimeMode() == core::RuntimeMode::realtime)
            scene_.invalidateAccumulation();
        ++frameCount;
        if (options_.frameLimit && frameCount >= *options_.frameLimit)
            running = false;
    }
    device->waitIdle();
    audioPlayer_.stop();
    log::info(frameProfiler_.report());
    log::info("Rendered ", frameCount, " frame(s); clean shutdown");
    return 0;
}

core::DayoProject Application::currentProject() const {
    core::DayoProject project;
    project.renderer = device_ == nullptr ? "preview" : std::string(graphics::toString(device_->activeRenderer()));
    project.frame = animationFrame_;
    project.playing = playing_;
    project.assets = projectAssets_;
    core::VmdMotion camera = scene_.cameraMotion() == nullptr ? core::VmdMotion{} : *scene_.cameraMotion();
    if (camera.modelName.empty())
        camera.modelName = "Camera/Light";
    project.embeddedMotions.push_back(std::move(camera));
    for (const auto& model : scene_.models()) {
        auto motion = model.motion == nullptr ? core::VmdMotion{} : *model.motion;
        if (motion.modelName.empty())
            motion.modelName = model.displayName;
        project.embeddedMotions.push_back(std::move(motion));
    }
    return project;
}

int Application::runVideoExport() {
    if (!options_.videoExport)
        throw std::invalid_argument("--export-video is required");
    const auto& options = *options_.videoExport;
    if (options.destination.empty())
        throw std::invalid_argument("--export-video requires a destination path");
    if (options.preferHardware) {
        if (!core::canExportVideoHardware(options.codec))
            throw std::runtime_error("requested VAAPI video encoder is unavailable");
    } else if (!core::canExportVideo(options.codec)) {
        throw std::runtime_error("requested video encoder is unavailable");
    }
    if (device_ == nullptr)
        throw std::logic_error("video export has no graphics device");

    std::optional<std::filesystem::path> audioSource;
    if (options.includeAudio) {
        if (options.audioSource) {
            const auto source = std::filesystem::absolute(*options.audioSource);
            core::MediaFile media(source);
            if (!media.info().hasAudio) {
                throw std::runtime_error("audio source has no audio stream: " + source.string());
            }
            audioSource = source;
        } else {
            std::vector<std::filesystem::path> candidates;
            for (const auto& asset : options_.assets) {
                const auto kind = core::classifyAsset(asset);
                if (kind != core::AssetKind::audio && kind != core::AssetKind::video)
                    continue;
                const auto source = std::filesystem::absolute(asset);
                core::MediaFile media(source);
                if (media.info().hasAudio &&
                    std::find(candidates.begin(), candidates.end(), source) == candidates.end()) {
                    candidates.push_back(source);
                }
            }
            if (candidates.size() > 1) {
                throw std::runtime_error("multiple audio sources found; use --audio-source PATH");
            }
            if (candidates.size() == 1)
                audioSource = candidates.front();
            else if (!audioSource_.empty())
                audioSource = audioSource_;
        }
        if (!audioSource) {
            log::warn("Video export: no audio source found; exporting video only");
        }
    }

    const auto timelineEnd =
        scene_.timeline().duration > 0.0F ? static_cast<std::uint64_t>(std::ceil(scene_.timeline().duration)) : 0U;
    const auto firstFrame = options.fromFrame.value_or(0U);
    const auto lastFrame = options.toFrame.value_or(timelineEnd);
    if (lastFrame < firstFrame)
        throw std::invalid_argument("video frame range is reversed");
    if (lastFrame == std::numeric_limits<std::uint64_t>::max()) {
        throw std::invalid_argument("video frame range is too large");
    }
    const auto sourceFps = sceneTimelineFps(scene_);
    const auto frameCount = videoOutputFrameCount(firstFrame, lastFrame, sourceFps, options.fps);

    core::VideoExportRequest request;
    request.destination = std::filesystem::absolute(options.destination);
    request.width = options.width;
    request.height = options.height;
    request.fps = options.fps;
    request.codec = options.codec;
    request.bitrate = options.bitrate;
    request.preferHardware = options.preferHardware;
    request.includeAudio = audioSource.has_value();
    request.audioBitrate = options.audioBitrate;
    request.overwrite = options.overwrite;
    request.audioStartSeconds = static_cast<double>(firstFrame) / sourceFps;

    core::VideoExporter exporter(request);
    if (audioSource) {
        core::MediaFile media(*audioSource);
        const auto maxSamples =
            static_cast<std::uint64_t>(std::ceil(static_cast<double>(frameCount) / options.fps * 48'000.0)) * 2U;
        std::uint64_t writtenSamples = 0;
        media.streamAudio(
            [&](std::span<const float> samples, std::uint32_t sampleRate, std::uint32_t channels) {
                if (writtenSamples >= maxSamples)
                    return;
                const auto count = std::min<std::uint64_t>(samples.size(), maxSamples - writtenSamples);
                exporter.writeAudio(samples.first(static_cast<std::size_t>(count)), sampleRate, channels);
                writtenSamples += count;
            },
            request.audioStartSeconds);
    }

    const auto sourceFrameDuration = static_cast<float>(1.0 / sourceFps);
    uploadedAnimationFrame_ = -1;
    auto evaluateFrame = [&](float frame, float deltaSeconds, bool initialUpload) {
        animationFrame_ = frame;
        scene_.setFrame(animationFrame_);
        if (videoMode_ && scene_.media() != nullptr) {
            mediaSeconds_ = std::max(0.0, static_cast<double>(frame) / sourceFps);
        }
        refreshAnimatedMesh(initialUpload, deltaSeconds);
        if (videoMode_)
            refreshVideoFrame();
        refreshPreviewScene();
    };

    // Physics needs a continuous pre-roll when the requested range starts
    // later than frame zero. Output samples themselves remain on the source
    // timeline, so a 60 FPS export does not play a 30 FPS motion twice as fast.
    if (firstFrame == 0U) {
        evaluateFrame(0.0F, 0.0F, true);
    } else {
        evaluateFrame(0.0F, 0.0F, true);
        for (std::uint64_t frame = 1; frame <= firstFrame; ++frame) {
            evaluateFrame(static_cast<float>(frame), sourceFrameDuration, false);
        }
    }
    auto previousSourceFrame = static_cast<float>(firstFrame);
    for (std::uint64_t outputFrame = 0; outputFrame < frameCount; ++outputFrame) {
        const auto sourceFrame = videoSourceFrame(outputFrame, firstFrame, lastFrame, sourceFps, options.fps);
        if (outputFrame != 0U) {
            const auto deltaSeconds = std::max(0.0F, sourceFrame - previousSourceFrame) * sourceFrameDuration;
            evaluateFrame(sourceFrame, deltaSeconds, false);
        }
        previousSourceFrame = sourceFrame;
        const auto image = device_->renderToImage({request.width, request.height});
        exporter.writeVideoFrame(image);
        if (outputFrame + 1U == frameCount || outputFrame == 0U ||
            (outputFrame + 1U) % std::max<std::uint64_t>(1U, frameCount / 20U) == 0U) {
            log::info("Video export: ", outputFrame + 1U, "/", frameCount, " frames");
        }
    }
    device_->waitIdle();
    const auto result = exporter.finish();
    log::info("Exported MP4: ", result.output.string(), " (", result.durationSeconds, " s, ", result.encodedFrames,
              " frames)");
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
            currentProjectPath_ = std::filesystem::absolute(path).lexically_normal();
#if DAYO_HAS_IMGUI
            const auto projectText = currentProjectPath_->string();
            const auto projectLength = std::min(projectText.size(), projectDestination_.size() - 1U);
            std::copy_n(projectText.data(), projectLength, projectDestination_.data());
            projectDestination_[projectLength] = '\0';
#endif
            resetProjectRuntimeState();
            if (project.renderer == "subayai")
                device_->selectRenderer(graphics::RendererKind::subayai);
            else if (project.renderer == "bdpt")
                device_->selectRenderer(graphics::RendererKind::bdpt);
            else
                device_->selectRenderer(graphics::RendererKind::preview);
            for (const auto& asset : project.assets)
                handleAsset(asset.path);
            if (project.embeddedMotions.size() > 1U) {
                scene_.attachMotion(project.embeddedMotions.front());
                const auto& models = scene_.models();
                const auto count = std::min(models.size(), project.embeddedMotions.size() - 1U);
                for (std::size_t index = 0; index < count; ++index) {
                    const auto& motion = project.embeddedMotions[index + 1U];
                    if (!motion.bones.empty() || !motion.morphs.empty() || !motion.ik.empty()) {
                        scene_.attachMotion(motion, models[index].id);
                    }
                }
                manualCamera_ = false;
            } else if (project.embeddedMotion) {
                scene_.attachMotion(*project.embeddedMotion, scene_.selectedModelId());
                manualCamera_ = false;
            }
            animationFrame_ = project.frame;
            scene_.setFrame(animationFrame_);
            playing_ = project.playing;
            if (!scene_.models().empty())
                refreshAnimatedMesh(false);
            lastAsset_ =
                "Project " + path.filename().string() + " — " + std::to_string(project.assets.size()) + " assets";
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
            if (selectedModel() == nullptr)
                throw std::runtime_error("VMdayo requires a selected model");
            scene_.attachMotion(document.motion, scene_.selectedModelId(), document.modelName);
            animationFrame_ = 0.0F;
            scene_.setFrame(animationFrame_);
            refreshAnimatedMesh(false);
            refreshPreviewScene();
            lastAsset_ = "VMdayo " + path.filename().string();
            projectAssets_.push_back({"vmdayo", std::filesystem::absolute(path)});
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
            const std::array<graphics::PreviewVertex, 4> vertices{{
                {{-1.0F, -1.0F, 0.0F}, {}, {0.0F, 1.0F}},
                {{1.0F, -1.0F, 0.0F}, {}, {1.0F, 1.0F}},
                {{1.0F, 1.0F, 0.0F}, {}, {1.0F, 0.0F}},
                {{-1.0F, 1.0F, 0.0F}, {}, {0.0F, 0.0F}},
            }};
            const std::array<std::uint32_t, 6> indices{0, 1, 2, 2, 3, 0};
            if (scene_.models().empty()) {
                device_->uploadPreviewMesh(vertices, indices);
                refreshPreviewBackground();
                device_->updatePreviewMaterials(std::span<const graphics::PreviewMaterial>{});
                device_->updatePreviewDraws(std::span<const graphics::PreviewDraw>{});
                graphics::PreviewScene preview;
                preview.cameraDistance = 2.42F;
                preview.screenSource = graphics::PreviewScene::ScreenSource::backgroundImage;
                device_->updatePreviewScene(preview);
            } else {
                refreshPreviewBackground();
                refreshPreviewTextures();
                refreshAnimatedMesh(true);
                refreshPreviewScene();
            }
            lastAsset_ = "Image " + path.filename().string() + " — " + std::to_string(image.width) + "x" +
                         std::to_string(image.height);
            projectAssets_.push_back({"image", std::filesystem::absolute(path)});
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
            if (scene_.models().empty() || scene_.selectedModelId() != modelId)
                throw std::logic_error("PMX model was not retained in the scene");
            log::info("PMX scene state: models=", scene_.models().size(), " selected=", scene_.selectedModelId());
            videoMode_ = scene_.media() != nullptr && scene_.media()->info().hasVideo;
            normalization_ = scene_.selectedModel()->normalization;
            refreshPreviewTextures();
            animationFrame_ = 0.0F;
            scene_.setFrame(animationFrame_);
            uploadedAnimationFrame_ = -1;
            refreshAnimatedMesh(true);
            if (videoMode_)
                refreshVideoFrame();
            refreshPreviewScene();
            const auto* model = selectedModel();
            lastAsset_ =
                "PMX " + model->model->metadata.modelName + " — v" + std::to_string(model->model->metadata.version) +
                ", vertices " + std::to_string(model->model->metadata.vertexCount) + ", triangles " +
                std::to_string(model->model->indices.size() / 3) + ", bones " +
                std::to_string(model->model->bones.size()) + ", models " + std::to_string(scene_.models().size());
            log::info("Loaded metadata: ", lastAsset_, " (", path.string(), ")");
            projectAssets_.push_back({"pmx", std::filesystem::absolute(path)});
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
            if (media->info().hasAudio && !options_.videoExport) {
                loadedAudio_ = media->decodeAudio();
                waveformPeaks_.assign(1024, 0.0F);
                if (!loadedAudio_.samples.empty() && loadedAudio_.channels != 0) {
                    const auto frames = loadedAudio_.samples.size() / loadedAudio_.channels;
                    for (std::size_t bucket = 0; bucket < waveformPeaks_.size(); ++bucket) {
                        const auto begin = bucket * frames / waveformPeaks_.size();
                        const auto end = std::max((bucket + 1) * frames / waveformPeaks_.size(), begin + 1);
                        float peak = 0.0F;
                        for (auto frame = begin; frame < std::min(end, frames); ++frame) {
                            for (std::uint32_t channel = 0; channel < loadedAudio_.channels; ++channel) {
                                peak = std::max(
                                    peak, std::abs(loadedAudio_.samples[frame * loadedAudio_.channels + channel]));
                            }
                        }
                        waveformPeaks_[bucket] = peak;
                    }
                }
                audioPlayer_.play(loadedAudio_, std::max(0.0F, audioOffsetSeconds_));
                audioPlayer_.setVolume(audioVolume_);
            }
            if (media->info().hasAudio) {
                audioSource_ = std::filesystem::absolute(path);
                setAudioExportDestinationForSource(path);
                audioToSeconds_ = static_cast<float>(std::max(0.0, media->info().durationSeconds));
            }
            if (videoMode_ && scene_.models().empty()) {
                const std::array<graphics::PreviewVertex, 4> vertices{{
                    {{-0.9F, -0.9F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F}},
                    {{0.9F, -0.9F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 1.0F}},
                    {{0.9F, 0.9F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F}},
                    {{-0.9F, 0.9F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F}},
                }};
                const std::array<std::uint32_t, 6> indices{0, 1, 2, 2, 3, 0};
                device_->uploadPreviewMesh(vertices, indices);
                refreshVideoFrame();
            } else if (!scene_.models().empty()) {
                refreshPreviewTextures();
                refreshAnimatedMesh(true);
                refreshVideoFrame();
            }
            lastAsset_ =
                std::string(core::toString(kind)) + " — " + std::to_string(media->info().durationSeconds) + " s";
            log::info("Loaded media: ", lastAsset_, " (", path.string(), ")");
            projectAssets_.push_back(
                {kind == core::AssetKind::audio ? "audio" : "video", std::filesystem::absolute(path)});
        } catch (const std::exception& exception) {
            lastAsset_ = "Media error: " + std::string(exception.what());
            log::warn(lastAsset_);
        }
        return;
    }
    if (kind == core::AssetKind::vmd) {
        try {
            auto motion = core::loadVmd(path);
            const bool cameraOnly =
                motion.bones.empty() && motion.morphs.empty() && (!motion.cameras.empty() || !motion.lights.empty());
            const auto motionName = motion.modelName;
            const auto boneKeyCount = motion.bones.size();
            const auto morphKeyCount = motion.morphs.size();
            const auto lastFrame = motion.lastFrame;
            const auto cameraKeyCount = motion.cameras.size();
            const auto lightKeyCount = motion.lights.size();
            scene_.attachMotion(std::move(motion));
            manualCamera_ = false;
            animationFrame_ = 0.0F;
            scene_.setFrame(animationFrame_);
            if (selectedModel() != nullptr)
                refreshAnimatedMesh(false);
            refreshPreviewScene();
            if (cameraOnly) {
                lastAsset_ = "VMD camera/light — " + std::to_string(cameraKeyCount) + " camera keys, " +
                             std::to_string(lightKeyCount) + " light keys, " + std::to_string(lastFrame) + " frames";
            } else {
                lastAsset_ = "VMD " + (motionName.empty() ? path.filename().string() : motionName) + " — " +
                             std::to_string(boneKeyCount) + " bone keys, " + std::to_string(morphKeyCount) +
                             " morph keys, " + std::to_string(lastFrame) + " frames";
            }
            log::info("Loaded motion: ", lastAsset_, " (", path.string(), ")");
            projectAssets_.push_back({"vmd", std::filesystem::absolute(path)});
        } catch (const std::exception& exception) {
            lastAsset_ = "VMD error: " + std::string(exception.what());
            log::warn(lastAsset_);
        }
        return;
    }
    if (kind == core::AssetKind::vpd) {
        try {
            scene_.attachPose(path);
            if (selectedModel() != nullptr)
                refreshAnimatedMesh(false);
            const auto* pose = selectedModel()->pose.get();
            lastAsset_ = "VPD pose — " + std::to_string(pose->bones.size()) + " bones";
            log::info("Loaded pose: ", lastAsset_, " (", path.string(), ")");
            projectAssets_.push_back({"vpd", std::filesystem::absolute(path)});
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
            if (effectReloader_->current() == nullptr)
                throw std::runtime_error("effect graph is empty");
            scene_.setEffect(*effectReloader_->current());
            auto filename = path.filename().string();
            std::ranges::transform(filename, filename.begin(),
                                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            if (device_ != nullptr && filename.find("subayai") != std::string::npos) {
                device_->selectRenderer(graphics::RendererKind::subayai);
            } else if (device_ != nullptr && filename.find("bdpt") != std::string::npos) {
                device_->selectRenderer(graphics::RendererKind::bdpt);
            }
            lastAsset_ = "Effect " + path.filename().string() + " — " + std::to_string(scene_.effect()->passes.size()) +
                         " passes, " + std::to_string(scene_.effect()->textures.size()) + " textures";
            log::info("Loaded effect graph: ", lastAsset_);
            projectAssets_.push_back({"effect", std::filesystem::absolute(path)});
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
    if (device_ == nullptr || scene_.models().empty())
        return;
    frameScratch_.reset();
    auto* scratch = frameScratch_.resource();
    std::size_t vertexCount = 0;
    std::size_t indexCount = 0;
    std::size_t materialCount = 0;
    bool dynamicVertices = false;
    for (const auto& instance : scene_.models()) {
        if (!instance.visible || instance.model == nullptr || instance.animator == nullptr)
            continue;
        vertexCount += instance.model->vertices.size();
        indexCount += instance.model->indices.size();
        materialCount += instance.model->materials.size();
        dynamicVertices = dynamicVertices || (instance.softBody != nullptr && instance.softBody->available());
    }
    const bool rebuildTopology = initialUpload || animatedTopologyGeneration_ != scene_.topologyGeneration() ||
                                 animatedIndices_.size() != indexCount ||
                                 animatedMaterialTemplates_.size() != materialCount ||
                                 animatedDraws_.size() != materialCount;
    const bool rebuildVertices = initialUpload || rebuildTopology || dynamicVertices;
    std::pmr::vector<graphics::PreviewVertex> vertices(scratch);
    vertices.reserve(vertexCount);
    std::pmr::vector<graphics::PreviewMorphDelta> morphDeltas(scratch);
    std::pmr::vector<float> morphWeights(scratch);
    morphWeights.reserve(materialCount);
    std::pmr::vector<graphics::PreviewMaterial> materials(scratch);
    std::pmr::vector<graphics::PreviewDraw> draws(scratch);
    if (!rebuildTopology) {
        materials.assign(animatedMaterialTemplates_.begin(), animatedMaterialTemplates_.end());
        draws.assign(animatedDraws_.begin(), animatedDraws_.end());
    }
    if (rebuildTopology) {
        animatedIndices_.clear();
        animatedIndices_.reserve(indexCount);
        animatedMorphDeltas_.clear();
        animatedMorphRanges_.clear();
        animatedMorphRanges_.reserve(vertexCount);
        materials.reserve(materialCount);
        draws.reserve(materialCount);
    }
    struct EvaluatedModel {
        const core::ModelInstance* instance{};
        bool gpuSkinning{};
        core::AnimatedModelFrame frame;
    };
    std::pmr::vector<EvaluatedModel> evaluated(scratch);
    evaluated.reserve(scene_.models().size());
    for (const auto& instance : scene_.models()) {
        if (!instance.visible || instance.model == nullptr || instance.animator == nullptr)
            continue;
        const auto gravity = scene_.evaluatePhysicsSettings(animationFrame_);
        if (instance.physics != nullptr) {
            instance.physics->setGravity({gravity.gravityDirection[0] * gravity.gravity,
                                          gravity.gravityDirection[1] * gravity.gravity,
                                          gravity.gravityDirection[2] * gravity.gravity});
            instance.physics->setGravityNoise(gravity.noiseAmplitude, gravity.noiseFrequency);
            instance.physics->setFloorCollision(gravity.floorCollision);
        }
        evaluated.push_back({&instance, instance.softBody == nullptr || !instance.softBody->available(), {}});
    }
    {
        auto animation = frameProfiler_.measure(core::ProfileSection::animation);
        taskScheduler_.parallelFor(evaluated.size(), [&](std::size_t index) {
            auto& current = evaluated[index];
            const auto& instance = *current.instance;
            current.frame = instance.animator->evaluate(animationFrame_, deltaSeconds, current.gpuSkinning);
            if (instance.softBody != nullptr && instance.softBody->available()) {
                const auto gravity = scene_.evaluatePhysicsSettings(animationFrame_);
                instance.softBody->step(deltaSeconds, {gravity.gravityDirection[0] * gravity.gravity,
                                                       gravity.gravityDirection[1] * gravity.gravity,
                                                       gravity.gravityDirection[2] * gravity.gravity});
                instance.softBody->apply(current.frame.vertices);
            }
            core::normalizeForPreview(current.frame.vertices, instance.normalization);
        });
        animation.finish();
    }
    std::size_t materialCursor = 0;
    std::uint32_t indexCursor = 0;
    std::pmr::vector<graphics::PreviewBoneTransform> bones(scratch);
    for (const auto& evaluatedModel : evaluated) {
        const auto& instance = *evaluatedModel.instance;
        const auto& frame = evaluatedModel.frame;
        const bool gpuSkinning = evaluatedModel.gpuSkinning;
        const auto morphWeightBase = static_cast<std::uint32_t>(morphWeights.size());
        for (std::size_t morphIndex = 0; morphIndex < instance.model->morphs.size(); ++morphIndex) {
            morphWeights.push_back(morphIndex < frame.morphWeights.size() ? frame.morphWeights[morphIndex] : 0.0F);
        }
        std::pmr::vector<std::array<std::uint32_t, 2>> sourceMorphRanges(scratch);
        if (rebuildTopology) {
            std::pmr::vector<std::uint32_t> morphCounts(scratch);
            std::pmr::vector<std::uint32_t> morphCursors(scratch);
            morphCounts.assign(instance.model->vertices.size(), 0U);
            for (const auto& morph : instance.model->morphs) {
                if (morph.type == 1) {
                    for (const auto& offset : morph.offsets) {
                        if (offset.index < 0 || static_cast<std::size_t>(offset.index) >= morphCounts.size())
                            continue;
                        ++morphCounts[static_cast<std::size_t>(offset.index)];
                    }
                }
            }
            sourceMorphRanges.resize(morphCounts.size());
            morphCursors.resize(morphCounts.size());
            const auto deltaBase = static_cast<std::uint32_t>(morphDeltas.size());
            std::uint32_t deltaOffset = deltaBase;
            for (std::size_t vertexIndex = 0; vertexIndex < morphCounts.size(); ++vertexIndex) {
                sourceMorphRanges[vertexIndex] = {deltaOffset, morphCounts[vertexIndex]};
                morphCursors[vertexIndex] = deltaOffset;
                deltaOffset += morphCounts[vertexIndex];
            }
            morphDeltas.resize(deltaOffset);
            std::size_t morphIndex = 0;
            for (const auto& morph : instance.model->morphs) {
                if (morph.type == 1) {
                    for (const auto& offset : morph.offsets) {
                        if (offset.index < 0 || static_cast<std::size_t>(offset.index) >= morphCursors.size())
                            continue;
                        graphics::PreviewMorphDelta delta;
                        for (std::size_t axis = 0; axis < 3; ++axis)
                            delta.delta[axis] = offset.vector3[axis] * instance.normalization.scale;
                        delta.morphIndex = morphWeightBase + static_cast<std::uint32_t>(morphIndex);
                        morphDeltas[morphCursors[static_cast<std::size_t>(offset.index)]++] = delta;
                    }
                }
                ++morphIndex;
            }
        }
        if (!frame.vertices.empty() && (initialUpload || static_cast<int>(animationFrame_) % 30 == 0)) {
            const auto& vertex = frame.vertices.front().position;
            log::debug("Animation sample: model=", instance.displayName, ", frame=", animationFrame_, ", vertex0=(",
                       vertex[0], ",", vertex[1], ",", vertex[2], ")");
        }
        const auto boneBase = static_cast<std::int32_t>(bones.size());
        if (gpuSkinning) {
            bones.reserve(bones.size() + frame.bones.size());
            for (const auto& source : frame.bones) {
                graphics::PreviewBoneTransform bone;
                std::copy(source.rotation.begin(), source.rotation.end(), bone.rotation);
                const auto rotatedCenter = rotateQuaternion(source.rotation, instance.normalization.center);
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    bone.translation[axis] =
                        (rotatedCenter[axis] + source.translation[axis] - instance.normalization.center[axis]) *
                        instance.normalization.scale;
                }
                bones.push_back(bone);
            }
        }
        const auto textureBase = [&] {
            std::size_t value = 0;
            for (const auto& previous : scene_.models()) {
                if (previous.id == instance.id)
                    break;
                value += previous.textures.size();
            }
            return static_cast<std::uint32_t>(value);
        }();
        const auto cloneCount = std::max(instance.cloneCount, 1U);
        const auto baseVertex = static_cast<std::uint32_t>(vertices.size());
        const auto firstModelIndex = indexCursor;
        if (rebuildVertices) {
            auto conversion = frameProfiler_.measure(core::ProfileSection::vertexConvert);
            for (std::size_t sourceIndex = 0; sourceIndex < frame.vertices.size(); ++sourceIndex) {
                const auto& source = frame.vertices[sourceIndex];
                graphics::PreviewVertex vertex;
                std::memcpy(vertex.position, source.position.data(), sizeof(vertex.position));
                std::memcpy(vertex.normal, source.normal.data(), sizeof(vertex.normal));
                std::memcpy(vertex.uv, source.uv.data(), sizeof(vertex.uv));
                const bool supported = gpuSkinning;
                if (supported) {
                    for (std::size_t influence = 0; influence < 4; ++influence) {
                        vertex.bones[influence] =
                            source.bones[influence] < 0 ||
                                    static_cast<std::size_t>(source.bones[influence]) >= frame.bones.size()
                                ? -1
                                : boneBase + source.bones[influence];
                        vertex.weights[influence] = source.weights[influence];
                    }
                    const auto normalizedC = normalizePreviewPoint(source.sdefC, instance.normalization);
                    const auto normalizedR0 = normalizePreviewPoint(source.sdefR0, instance.normalization);
                    const auto normalizedR1 = normalizePreviewPoint(source.sdefR1, instance.normalization);
                    std::copy(normalizedC.begin(), normalizedC.end(), vertex.sdefC);
                    for (std::size_t axis = 0; axis < 3; ++axis)
                        vertex.sdefHalfDelta[axis] = (normalizedR0[axis] - normalizedR1[axis]) * 0.5F;
                    vertex.skinningType = static_cast<std::uint32_t>(source.weightType);
                    vertex.gpuSkinning = 1;
                }
                vertex.edgeScale = source.edgeScale;
                const auto morphRange =
                    rebuildTopology ? sourceMorphRanges[sourceIndex] : animatedMorphRanges_[baseVertex + sourceIndex];
                vertex.morphStart = morphRange[0];
                vertex.morphCount = gpuSkinning ? morphRange[1] : 0U;
                vertices.push_back(vertex);
                if (rebuildTopology)
                    animatedMorphRanges_.push_back(morphRange);
            }
            conversion.finish();
        }
        if (rebuildTopology) {
            for (const auto index : instance.model->indices)
                animatedIndices_.push_back(baseVertex + index);
        }
        indexCursor += static_cast<std::uint32_t>(instance.model->indices.size());
        std::uint32_t firstIndex = firstModelIndex;
        for (std::size_t materialIndex = 0; materialIndex < instance.model->materials.size(); ++materialIndex) {
            const auto& sourceMaterial = instance.model->materials[materialIndex];
            if (rebuildTopology) {
                graphics::PreviewMaterial material;
                material.doubleSided = (sourceMaterial.drawFlags & 0x01U) != 0;
                material.edgeEnabled = (sourceMaterial.drawFlags & 0x10U) != 0;
                material.textureSlot = sourceMaterial.textureIndex >= 0
                                           ? textureBase + static_cast<std::uint32_t>(sourceMaterial.textureIndex) + 1U
                                           : 0U;
                const bool hasSphereTexture = hasLoadedTexture(instance.textures, sourceMaterial.sphereTextureIndex);
                material.sphereTextureSlot =
                    hasSphereTexture ? textureBase + static_cast<std::uint32_t>(sourceMaterial.sphereTextureIndex) + 1U
                                     : 0U;
                material.toonTextureSlot =
                    sourceMaterial.toonMode == 0 && sourceMaterial.toonTextureIndex >= 0
                        ? textureBase + static_cast<std::uint32_t>(sourceMaterial.toonTextureIndex) + 1U
                        : 0U;
                material.sphereMode = hasSphereTexture ? sourceMaterial.sphereMode : 0U;
                material.toonMode = sourceMaterial.toonMode;
                materials.push_back(material);
                draws.push_back({});
            }
            auto& material = materials[materialCursor++];
            if (materialIndex < frame.materials.size()) {
                const auto& animated = frame.materials[materialIndex];
                std::copy(animated.diffuse.begin(), animated.diffuse.end(), material.diffuse);
                std::copy(animated.ambient.begin(), animated.ambient.end(), material.ambient);
                std::copy(animated.specular.begin(), animated.specular.end(), material.specular);
                material.shininess = animated.shininess;
                std::copy(animated.textureMultiply.begin(), animated.textureMultiply.end(), material.textureMultiply);
                std::copy(animated.textureAdd.begin(), animated.textureAdd.end(), material.textureAdd);
                std::copy(animated.sphereMultiply.begin(), animated.sphereMultiply.end(), material.sphereMultiply);
                std::copy(animated.sphereAdd.begin(), animated.sphereAdd.end(), material.sphereAdd);
                std::copy(animated.toonMultiply.begin(), animated.toonMultiply.end(), material.toonMultiply);
                std::copy(animated.toonAdd.begin(), animated.toonAdd.end(), material.toonAdd);
                std::copy(animated.edgeColor.begin(), animated.edgeColor.end(), material.edgeColor);
                // Edge extrusion is a distance in the same normalized space as the vertices.
                material.edgeSize = animated.edgeSize * instance.normalization.scale;
            }
            auto& draw = draws[materialCursor - 1U];
            draw.firstIndex = firstIndex;
            draw.indexCount = sourceMaterial.indexCount;
            draw.materialIndex = static_cast<std::uint32_t>(materialCursor - 1U);
            draw.instanceCount = cloneCount;
            firstIndex += instance.model->materials[materialIndex].indexCount;
        }
    }
    if ((rebuildVertices && vertices.empty()) || animatedIndices_.empty())
        return;
    if (rebuildTopology) {
        animatedMaterialTemplates_.assign(materials.begin(), materials.end());
        animatedDraws_.assign(draws.begin(), draws.end());
        animatedMorphDeltas_.assign(morphDeltas.begin(), morphDeltas.end());
        animatedTopologyGeneration_ = scene_.topologyGeneration();
    }
    {
        auto upload = frameProfiler_.measure(core::ProfileSection::upload);
        device_->updatePreviewBones(bones);
        if (initialUpload || rebuildTopology)
            device_->uploadPreviewMorphDeltas(morphDeltas);
        device_->updatePreviewMorphWeights(morphWeights);
        if (initialUpload || rebuildTopology)
            device_->uploadPreviewMesh(vertices, animatedIndices_);
        else if (dynamicVertices) {
            try {
                device_->updatePreviewVertices(vertices);
            } catch (const std::exception&) {
                device_->uploadPreviewMesh(vertices, animatedIndices_);
            }
        }
        device_->updatePreviewMaterials(materials);
        if (initialUpload || rebuildTopology)
            device_->updatePreviewDraws(draws);
        upload.finish();
    }
    const auto byteCount = [](std::size_t count, std::size_t elementSize) {
        return static_cast<std::uint64_t>(count) * static_cast<std::uint64_t>(elementSize);
    };
    std::uint64_t uploadBytes = byteCount(bones.size(), sizeof(graphics::PreviewBoneTransform));
    uploadBytes += byteCount(materials.size(), sizeof(graphics::PreviewMaterial));
    uploadBytes += byteCount(draws.size(), sizeof(graphics::PreviewDraw));
    if (rebuildVertices)
        uploadBytes += byteCount(vertices.size(), sizeof(graphics::PreviewVertex));
    if (initialUpload || rebuildTopology)
        uploadBytes += byteCount(morphDeltas.size(), sizeof(graphics::PreviewMorphDelta));
    uploadBytes += byteCount(morphWeights.size(), sizeof(float));
    if (initialUpload || rebuildTopology)
        uploadBytes += byteCount(animatedIndices_.size(), sizeof(std::uint32_t));
    frameProfiler_.addUploadBytes(uploadBytes);
    if (rebuildVertices)
        animatedVertexCount_ = static_cast<std::uint64_t>(vertices.size());
    uploadedAnimationFrame_ = static_cast<int>(animationFrame_);
    scene_.clearDirty(core::DirtyFlag::geometry | core::DirtyFlag::material);
}

void Application::refreshPreviewTextures() {
    if (device_ == nullptr)
        return;
    textures_.clear();
    for (const auto& instance : scene_.models()) {
        textures_.insert(textures_.end(), instance.textures.begin(), instance.textures.end());
    }
    std::vector<graphics::PreviewTexture> previewTextures;
    previewTextures.reserve(textures_.size());
    for (const auto& texture : textures_) {
        previewTextures.push_back({texture.width, texture.height, texture.pixels, hasTransparentPixels(texture)});
    }
    device_->uploadPreviewTextures(previewTextures);
}

void Application::refreshPreviewBackground() {
    if (device_ == nullptr)
        return;
    const auto& background = scene_.background();
    uploadedVideoFrame_ = -1;
    if (background.screenSource == core::ScreenTextureSource::backgroundVideo) {
        if (videoMode_ && scene_.media() != nullptr && scene_.media()->info().hasVideo)
            refreshVideoFrame();
        else
            device_->uploadPreviewBackground({});
        return;
    }
    if (background.screenSource != core::ScreenTextureSource::backgroundImage || !background.image.has_value()) {
        device_->uploadPreviewBackground({});
        return;
    }
    const auto& image = *background.image;
    const std::array textures{graphics::PreviewTexture{image.width, image.height, image.pixels}};
    device_->uploadPreviewBackground(textures);
}

void Application::refreshVideoFrame() {
    auto* media = scene_.media();
    if (!videoMode_ || media == nullptr || device_ == nullptr ||
        scene_.background().screenSource != core::ScreenTextureSource::backgroundVideo)
        return;
    const auto frameIndex = static_cast<std::int64_t>(mediaSeconds_ * media->info().videoFramesPerSecond);
    if (frameIndex == uploadedVideoFrame_)
        return;
    const auto image = media->decodeVideoFrame(mediaSeconds_);
    const std::array textures{graphics::PreviewTexture{image.width, image.height, image.pixels}};
    device_->uploadPreviewBackground(textures);
    if (scene_.models().empty()) {
        device_->updatePreviewMaterials({});
        device_->updatePreviewDraws({});
    }
    uploadedVideoFrame_ = frameIndex;
}

void Application::refreshPreviewScene() {
    if (device_ == nullptr)
        return;
    const auto* model = selectedModel();
    const auto* motion =
        scene_.cameraMotion() != nullptr ? scene_.cameraMotion() : (model != nullptr ? model->motion.get() : nullptr);
    graphics::PreviewScene scene;
    switch (scene_.background().screenSource) {
    case core::ScreenTextureSource::previousFrame:
        scene.screenSource = graphics::PreviewScene::ScreenSource::previousFrame;
        break;
    case core::ScreenTextureSource::backgroundVideo:
        scene.screenSource = graphics::PreviewScene::ScreenSource::backgroundVideo;
        break;
    case core::ScreenTextureSource::backgroundImage:
        scene.screenSource = graphics::PreviewScene::ScreenSource::backgroundImage;
        break;
    case core::ScreenTextureSource::white:
        scene.screenSource = graphics::PreviewScene::ScreenSource::white;
        break;
    }
    scene.screenCrop = scene_.background().crop == core::ScreenCropMode::crop4x3
                           ? graphics::PreviewScene::ScreenCrop::crop4x3
                           : graphics::PreviewScene::ScreenCrop::none;
    scene.backgroundEnabled = scene_.background().enabled;
    scene.cameraRotation[0] = cameraPitch_;
    scene.cameraRotation[1] = cameraYaw_;
    scene.cameraDistance = cameraDistance_;
    scene.debugMaterial = previewDebugMaterial_;
    scene.debugFlags = previewDebugFlags_;
    scene.outlineEnabled = previewOutlineEnabled_;
    if (!manualCamera_ && motion != nullptr && !motion->cameras.empty()) {
        const auto camera = core::evaluateCamera(*motion, animationFrame_);
        std::copy(camera.rotation.begin(), camera.rotation.end(), scene.cameraRotation);
        const auto normalization = model != nullptr ? model->normalization : normalization_;
        scene.cameraDistance = std::max(std::abs(camera.distance) * normalization.scale, 0.4F);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            scene.target[axis] = (camera.position[axis] - normalization.center[axis]) * normalization.scale;
        }
        scene.verticalFovRadians = std::clamp(camera.viewAngle, 1.0F, 179.0F) * 0.01745329252F;
        scene.perspective = camera.perspective;
    }
    if (motion != nullptr && !motion->lights.empty()) {
        const auto light = core::evaluateLight(*motion, animationFrame_);
        std::copy(light.position.begin(), light.position.end(), scene.lightDirection);
        std::copy(light.color.begin(), light.color.end(), scene.lightColor);
    }
    device_->updatePreviewScene(scene);
}

void Application::resetPhysicsSimulation() {
    for (auto& instance : scene_.models()) {
        if (instance.physics != nullptr)
            (*instance.physics).reset();
        if (instance.softBody != nullptr && instance.softBody->available())
            (*instance.softBody).reset();
    }
}

void Application::evaluateExportFrame(float frame, float deltaSeconds, bool initialUpload) {
    animationFrame_ = frame;
    scene_.setFrame(frame);
    if (videoMode_ && scene_.media() != nullptr) {
        mediaSeconds_ = std::max(0.0, static_cast<double>(frame) / sceneTimelineFps(scene_));
        refreshVideoFrame();
    }
    refreshAnimatedMesh(initialUpload, deltaSeconds);
    refreshPreviewScene();
}

bool Application::advanceDeterministicFrameEvaluation(float targetFrame, std::uint64_t& nextFrame) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
    const float target = std::max(targetFrame, 0.0F);
    const float frameDuration = static_cast<float>(1.0 / sceneTimelineFps(scene_));
    const auto wholeFrames = static_cast<std::uint64_t>(std::floor(target));
    if (nextFrame == 0) {
        resetPhysicsSimulation();
        evaluateExportFrame(0.0F, 0.0F, true);
        nextFrame = 1;
    }
    while (nextFrame <= wholeFrames && std::chrono::steady_clock::now() < deadline) {
        evaluateExportFrame(static_cast<float>(nextFrame), frameDuration);
        ++nextFrame;
    }
    if (nextFrame <= wholeFrames)
        return false;
    const float fraction = target - static_cast<float>(wholeFrames);
    if (fraction > 0.0F)
        evaluateExportFrame(target, fraction * frameDuration);
    return true;
}

void Application::buildUi() {
#if DAYO_HAS_IMGUI
    uiState_.viewportHovered = false;
    uiState_.timelineFocused = false;
    buildMainMenuBar();
    buildStatusBar();
    buildDockLayout();
    auto* model = selectedModel();
    if (ImGui::Begin(workspaceWindowName("Viewport", "viewport").c_str(), nullptr,
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        if (available.x > 0.0F && available.y > 0.0F) {
            const auto* imguiViewport = ImGui::GetWindowViewport();
            const float dpiScale = std::max(imguiViewport->DpiScale, 1.0F);
            const auto pixelWidth = static_cast<std::uint32_t>(std::max(1.0F, std::round(available.x * dpiScale)));
            const auto pixelHeight = static_cast<std::uint32_t>(std::max(1.0F, std::round(available.y * dpiScale)));
            device_->setPreviewViewportExtent({pixelWidth, pixelHeight});
            const auto preview = device_->previewViewport();
            if (preview) {
                const auto imagePosition = ImGui::GetCursorScreenPos();
                ImGui::Image(ImTextureRef{static_cast<ImTextureID>(preview.textureId)}, available);
                const bool imageHovered = ImGui::IsItemHovered();
                if (scene_.models().empty()) {
                    constexpr auto message = "No model loaded\nDrop a PMX file into the window";
                    const auto messageSize = ImGui::CalcTextSize(message);
                    const ImVec2 padding{ImGui::GetStyle().FramePadding.x * 2.0F,
                                         ImGui::GetStyle().FramePadding.y * 2.0F};
                    const ImVec2 messagePosition{imagePosition.x + (available.x - messageSize.x) * 0.5F,
                                                 imagePosition.y + (available.y - messageSize.y) * 0.5F};
                    auto* drawList = ImGui::GetWindowDrawList();
                    drawList->AddRectFilled(
                        {messagePosition.x - padding.x, messagePosition.y - padding.y},
                        {messagePosition.x + messageSize.x + padding.x, messagePosition.y + messageSize.y + padding.y},
                        ImGui::GetColorU32(ImGuiCol_WindowBg, 0.88F), ImGui::GetStyle().FrameRounding);
                    drawList->AddText(messagePosition, ImGui::GetColorU32(ImGuiCol_TextDisabled), message);
                }
                bool overlayHovered = false;
                if (manualCamera_) {
                    ImGui::SetCursorScreenPos({imagePosition.x + ImGui::GetStyle().ItemSpacing.x,
                                               imagePosition.y + ImGui::GetStyle().ItemSpacing.y});
                    if (ImGui::Button("Use VMD camera")) {
                        manualCamera_ = false;
                        refreshPreviewScene();
                    }
                    overlayHovered = ImGui::IsItemHovered();
                }
                uiState_.viewportHovered = imageHovered && !overlayHovered;
            }
        } else {
            device_->setPreviewViewportExtent({});
        }
        const auto& io = ImGui::GetIO();
        const bool viewportInput = uiState_.viewportHovered && !ImGui::IsAnyItemActive();
        bool cameraChanged = false;
        if (viewportInput && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            cameraYaw_ += io.MouseDelta.x * 0.008F;
            cameraPitch_ = std::clamp(cameraPitch_ + io.MouseDelta.y * 0.008F, -1.5F, 1.5F);
            cameraChanged = true;
        }
        if (viewportInput && io.MouseWheel != 0.0F) {
            cameraDistance_ = std::clamp(cameraDistance_ * std::exp(-io.MouseWheel * 0.12F), 0.4F, 30.0F);
            cameraChanged = true;
        }
        if (cameraChanged) {
            manualCamera_ = true;
            refreshPreviewScene();
        }
    } else {
        device_->setPreviewViewportExtent({});
    }
    ImGui::End();

    if (uiState_.performanceVisible && ImGui::Begin(workspaceWindowName("Performance", "performance").c_str())) {
        const auto& totals = frameProfiler_.totals();
        ImGui::TextUnformatted(frameProfiler_.report().c_str());
        ImGui::Text("Upload: %.2f MiB/frame", totals.frames == 0 ? 0.0
                                                                 : static_cast<double>(totals.uploadBytes) /
                                                                       static_cast<double>(totals.frames) / 1048576.0);
        ImGui::Text("Geometry: %llu vertices / %llu draws per frame",
                    static_cast<unsigned long long>(totals.frames == 0 ? 0 : totals.vertices / totals.frames),
                    static_cast<unsigned long long>(totals.frames == 0 ? 0 : totals.draws / totals.frames));
        if (ImGui::Button("Reset profiler"))
            frameProfiler_.reset();
    }
    if (uiState_.performanceVisible)
        ImGui::End();

    if (uiState_.sceneVisible && ImGui::Begin(workspaceWindowName("Scene", "scene").c_str())) {
        if (ImGui::Selectable("Scene / Background", uiState_.inspectScene))
            uiState_.inspectScene = true;
        ImGui::InputTextWithHint("##scene-filter", "Search scene...", uiState_.sceneFilter.data(),
                                 uiState_.sceneFilter.size());
        ImGui::SeparatorText("Models");
        for (const auto& instance : scene_.models()) {
            if (uiState_.sceneFilter[0] != '\0' &&
                instance.displayName.find(uiState_.sceneFilter.data()) == std::string::npos)
                continue;
            ImGui::PushID(static_cast<int>(instance.id));
            bool selected = !uiState_.inspectScene && scene_.selectedModelId() == instance.id;
            const auto label = std::string(instance.visible ? "[visible]  " : "[hidden]  ") + instance.displayName;
            if (ImGui::Selectable(label.c_str(), selected)) {
                uiState_.inspectScene = false;
                scene_.selectModel(instance.id);
                normalization_ = instance.normalization;
                refreshAnimatedMesh(true);
                refreshPreviewScene();
            }
            if (ImGui::BeginPopupContextItem("scene-item-context")) {
                ImGui::MenuItem("Rename", nullptr, false, false);
                ImGui::MenuItem("Duplicate / Clone", nullptr, false, false);
                if (ImGui::MenuItem("Open Source Folder")) {
                    const auto url = "file://" + instance.sourcePath.parent_path().generic_string();
                    if (!SDL_OpenURL(url.c_str()))
                        lastAsset_ = std::string("Open folder: ") + SDL_GetError();
                }
                ImGui::MenuItem("Set External Parent", nullptr, false, false);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    if (uiState_.sceneVisible)
        ImGui::End();

    if (uiState_.physicsVisible && ImGui::Begin(workspaceWindowName("Physics", "physics").c_str())) {
        ImGui::Text("Timeline: %.1f / %.1f frames", animationFrame_, scene_.timeline().duration);
        ImGui::Checkbox("Repeat", &repeat_);
        ImGui::SliderFloat("Playback speed", &playbackSpeed_, 0.1F, 4.0F, "%.2fx");
        if (ImGui::SliderFloat("Volume", &audioVolume_, 0.0F, 1.0F))
            audioPlayer_.setVolume(audioVolume_);
        if (ImGui::DragFloat("Audio offset", &audioOffsetSeconds_, 0.01F, -60.0F, 60.0F, "%.2f s") &&
            !loadedAudio_.samples.empty()) {
            audioPlayer_.play(loadedAudio_, std::max(0.0F, audioOffsetSeconds_));
            audioPlayer_.setVolume(audioVolume_);
            audioPlayer_.setPaused(!playing_);
        }
        if (!waveformPeaks_.empty()) {
            ImGui::PlotLines("Waveform", waveformPeaks_.data(), static_cast<int>(waveformPeaks_.size()), 0, nullptr,
                             0.0F, 1.0F, {0.0F, 72.0F});
        }
        auto settings = scene_.physicsSettings();
        bool physicsChanged = ImGui::DragFloat("Gravity", &settings.gravity, 0.01F, 0.0F, 100.0F);
        physicsChanged |= ImGui::DragFloat3("Gravity direction", settings.gravityDirection.data(), 0.01F);
        physicsChanged |= ImGui::DragFloat("Gravity noise amplitude", &settings.noiseAmplitude, 0.01F, 0.0F, 100.0F);
        physicsChanged |= ImGui::DragFloat("Gravity noise frequency", &settings.noiseFrequency, 0.01F, 0.0F, 100.0F);
        physicsChanged |= ImGui::Checkbox("Floor collision", &settings.floorCollision);
        if (physicsChanged)
            scene_.setPhysicsSettings(settings);
        int runtimeMode = static_cast<int>(scene_.runtimeMode());
        if (ImGui::Combo("Runtime mode", &runtimeMode, "Accumulate\0Realtime\0Idle\0")) {
            history_.execute(scene_, std::make_unique<core::SetRuntimeModeCommand>(
                                         scene_.runtimeMode(), static_cast<core::RuntimeMode>(runtimeMode)));
        }
        ImGui::Text("Accumulated samples: %llu", static_cast<unsigned long long>(scene_.accumulatedSamples()));
        if (ImGui::Button("Reset physics") && model != nullptr && model->physics != nullptr)
            model->physics->reset();
        ImGui::SameLine();
        if (ImGui::Button("Update one frame") && model != nullptr) {
            refreshAnimatedMesh(false, 1.0F / 30.0F);
            refreshPreviewScene();
        }
        ImGui::Checkbox("Rigid body debug", &physicsDebug_);
        if (physicsDebug_ && model != nullptr && model->physics != nullptr) {
            const auto count = model->physics->bodyCount();
            if (ImGui::BeginChild("rigid-body-debug", {0.0F, 120.0F}, true)) {
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(count));
                while (clipper.Step())
                    for (int body = clipper.DisplayStart; body < clipper.DisplayEnd; ++body) {
                        const auto transform = model->physics->bodyTransform(static_cast<std::size_t>(body));
                        ImGui::Text("%d  mode %u  P %.2f %.2f %.2f", body,
                                    model->physics->bodyMode(static_cast<std::size_t>(body)), transform.position[0],
                                    transform.position[1], transform.position[2]);
                    }
            }
            ImGui::EndChild();
        }
    }
    if (uiState_.physicsVisible)
        ImGui::End();
    buildInspectorPanel();
    buildEditorUi();
    handleEditorShortcuts();
    buildAudioExportUi();
    buildVideoExportUi();
    buildImageSequenceExportUi();
    buildSaveAsDialog();
#endif
}

void Application::handleEditorShortcuts() {
#if DAYO_HAS_IMGUI
    const auto& io = ImGui::GetIO();
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
        saveProjectNow();
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false) && history_.undo(scene_)) {
        animationFrame_ = scene_.timeline().frame;
        refreshAnimatedMesh(false);
        refreshPreviewScene();
    }
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false) && history_.redo(scene_)) {
        animationFrame_ = scene_.timeline().frame;
        refreshAnimatedMesh(false);
        refreshPreviewScene();
    }
    const bool editorShortcutScope = uiState_.viewportHovered || uiState_.timelineFocused;
    if (!editorShortcutScope || io.WantTextInput)
        return;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        if (recordCamera_) {
            core::VmdMotion before = scene_.cameraMotion() ? *scene_.cameraMotion() : core::VmdMotion{};
            auto document = core::toMotionDocument(before);
            core::VmdCameraKey key = editedCamera_;
            key.frame = static_cast<std::uint32_t>(std::max(animationFrame_, 0.0F));
            key.distance = -cameraDistance_ / std::max(normalization_.scale, 0.0001F);
            key.rotation = {cameraPitch_, cameraYaw_, 0.0F};
            std::erase_if(document.cameras, [&](const auto& item) { return item.frame == key.frame; });
            document.cameras.push_back(key);
            core::MotionEditor::normalize(document);
            history_.execute(scene_, std::make_unique<core::EditMotionCommand>(
                                         0, true, before, core::toVmdMotion(std::move(document), before.modelName),
                                         "Record camera key"));
        } else {
            playing_ = !playing_;
            if (audioPlayer_.active())
                audioPlayer_.setPaused(!playing_);
        }
    }
#endif
}

void Application::setWorkspace(ui::Workspace workspace) {
#if DAYO_HAS_IMGUI
    auto& previousPanels = uiState_.workspacePanels[static_cast<std::size_t>(uiState_.workspace)];
    previousPanels.sceneVisible = uiState_.sceneVisible;
    previousPanels.inspectorVisible = uiState_.inspectorVisible;
    previousPanels.timelineVisible = uiState_.timelineVisible;
    uiState_.workspace = workspace;
    const auto& panels = uiState_.workspacePanels[static_cast<std::size_t>(workspace)];
    uiState_.sceneVisible = panels.sceneVisible;
    uiState_.inspectorVisible = panels.inspectorVisible;
    uiState_.timelineVisible = panels.timelineVisible;
    uiState_.performanceVisible = workspace == ui::Workspace::debug;
    uiState_.fxDebugVisible = workspace == ui::Workspace::debug;
    uiState_.materialDebugVisible = workspace == ui::Workspace::debug;
    uiState_.physicsVisible = workspace == ui::Workspace::debug;
#else
    static_cast<void>(workspace);
#endif
}

void Application::buildMainMenuBar() {
#if DAYO_HAS_IMGUI
    if (!ImGui::BeginMainMenuBar())
        return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save", "Ctrl+S"))
            saveProjectNow();
        if (ImGui::MenuItem("Save As..."))
            uiState_.saveAsOpen = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Alt+F4")) {
            SDL_Event event{};
            event.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&event);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const bool canUndo = history_.canUndo();
        const bool canRedo = history_.canRedo();
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo) && history_.undo(scene_)) {
            animationFrame_ = scene_.timeline().frame;
            refreshAnimatedMesh(false);
            refreshPreviewScene();
        }
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo) && history_.redo(scene_)) {
            animationFrame_ = scene_.timeline().frame;
            refreshAnimatedMesh(false);
            refreshPreviewScene();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Scene", nullptr, &uiState_.sceneVisible))
            uiState_.resetLayoutRequested = true;
        if (ImGui::MenuItem("Inspector", nullptr, &uiState_.inspectorVisible))
            uiState_.resetLayoutRequested = true;
        if (ImGui::MenuItem("Timeline", nullptr, &uiState_.timelineVisible))
            uiState_.resetLayoutRequested = true;
        ImGui::MenuItem("Status bar", nullptr, &uiState_.statusBarVisible);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) {
            uiState_.resetLayoutRequested = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Animation")) {
        if (ImGui::MenuItem(playing_ ? "Pause" : "Play", "Space")) {
            playing_ = !playing_;
            if (audioPlayer_.active())
                audioPlayer_.setPaused(!playing_);
        }
        ImGui::MenuItem("Loop", nullptr, &repeat_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Render")) {
        if (ImGui::MenuItem("Export Video..."))
            uiState_.videoExportOpen = true;
        if (ImGui::MenuItem("Export Audio..."))
            uiState_.audioExportOpen = true;
        if (ImGui::MenuItem("Export Image Sequence...")) {
            uiState_.imageSequenceExportOpen = true;
            setWorkspace(ui::Workspace::render);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::TextUnformatted("Space  Play / Pause");
        ImGui::TextUnformatted("Ctrl+Z / Ctrl+Y  Undo / Redo");
        ImGui::TextUnformatted("Right-drag / Wheel  Viewport camera");
        ImGui::EndMenu();
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Workspace");
    const auto workspaceButton = [&](const char* label, ui::Workspace workspace) {
        ImGui::SameLine();
        if (ImGui::Selectable(label, uiState_.workspace == workspace, ImGuiSelectableFlags_DontClosePopups))
            setWorkspace(workspace);
    };
    workspaceButton("Layout", ui::Workspace::layout);
    workspaceButton("Animation", ui::Workspace::animation);
    workspaceButton("Camera", ui::Workspace::camera);
    workspaceButton("Render", ui::Workspace::render);
    workspaceButton("Debug", ui::Workspace::debug);
    ImGui::EndMainMenuBar();
#endif
}

void Application::buildDockLayout() {
#if DAYO_HAS_IMGUI
    auto* viewport = ImGui::GetMainViewport();
    const char* workspaceId = workspaceSuffix(uiState_.workspace);
    const ImGuiID dockspaceId = ImGui::GetID((std::string("DayoEditorDockSpace.") + workspaceId).c_str());
    ImGui::DockSpaceOverViewport(dockspaceId, viewport, ImGuiDockNodeFlags_None);
    const auto* node = ImGui::DockBuilderGetNode(dockspaceId);
    const bool hasLayout = node != nullptr && (node->IsSplitNode() || node->Windows.Size > 0);
    if (hasLayout && !uiState_.resetLayoutRequested)
        return;

    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);
    ImGuiID right{};
    ImGuiID main = dockspaceId;
    const float inspectorRatio = uiState_.workspace == ui::Workspace::camera ? 0.30F : 0.25F;
    const float timelineRatio = uiState_.workspace == ui::Workspace::animation ? 0.40F : 0.30F;
    ImGuiID timeline{};
    if (uiState_.inspectorVisible)
        ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, inspectorRatio, &right, &main);
    if (uiState_.timelineVisible) {
        ImGuiID center{};
        ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, timelineRatio, &timeline, &center);
        main = center;
    }
    ImGuiID scene{};
    if (uiState_.sceneVisible) {
        ImGuiID viewportNode{};
        ImGui::DockBuilderSplitNode(main, ImGuiDir_Left, 0.18F, &scene, &viewportNode);
        main = viewportNode;
    }
    ImGui::DockBuilderDockWindow(workspaceWindowName("Viewport", "viewport").c_str(), main);
    if (uiState_.sceneVisible)
        ImGui::DockBuilderDockWindow(workspaceWindowName("Scene", "scene").c_str(), scene);
    if (uiState_.inspectorVisible)
        ImGui::DockBuilderDockWindow(workspaceWindowName("Inspector", "inspector").c_str(), right);
    if (uiState_.timelineVisible)
        ImGui::DockBuilderDockWindow(workspaceWindowName("Timeline", "timeline").c_str(), timeline);
    const ImGuiID auxiliaryNode = uiState_.inspectorVisible ? right : main;
    if (uiState_.physicsVisible)
        ImGui::DockBuilderDockWindow(workspaceWindowName("Physics", "physics").c_str(),
                                     uiState_.timelineVisible ? timeline : main);
    if (uiState_.performanceVisible)
        ImGui::DockBuilderDockWindow(workspaceWindowName("Performance", "performance").c_str(), auxiliaryNode);
    if (uiState_.fxDebugVisible)
        ImGui::DockBuilderDockWindow(workspaceWindowName("FX Debug", "fx-debug").c_str(), auxiliaryNode);
    if (uiState_.materialDebugVisible)
        ImGui::DockBuilderDockWindow(workspaceWindowName("Preview Material Inspector", "material-debug").c_str(),
                                     auxiliaryNode);
    if (uiState_.workspace == ui::Workspace::debug) {
        ImGui::DockBuilderDockWindow(workspaceWindowName("Bone / Expression", "legacy-bone").c_str(), auxiliaryNode);
        ImGui::DockBuilderDockWindow(workspaceWindowName("Project tools", "legacy-project").c_str(), auxiliaryNode);
    }
    if (uiState_.workspace == ui::Workspace::camera || uiState_.workspace == ui::Workspace::debug)
        ImGui::DockBuilderDockWindow(workspaceWindowName("Camera / Light / Self Shadow", "legacy-camera").c_str(),
                                     auxiliaryNode);
    ImGui::DockBuilderFinish(dockspaceId);
    uiState_.resetLayoutRequested = false;
#endif
}

void Application::buildInspectorPanel() {
#if DAYO_HAS_IMGUI
    if (!uiState_.inspectorVisible)
        return;
    auto* model = selectedModel();
    if (!ImGui::Begin(workspaceWindowName("Inspector", "inspector").c_str())) {
        ImGui::End();
        return;
    }
    if (uiState_.inspectScene || model == nullptr || model->model == nullptr) {
        ImGui::TextUnformatted("Scene settings");
        if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto background = scene_.background();
            int source = static_cast<int>(background.screenSource);
            if (ImGui::BeginTable("scene-properties", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 10.0F);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted("Background");
                ImGui::TableSetColumnIndex(1);
                if (ImGui::Combo("##background-source", &source, "Previous frame\0Video\0Image\0White\0")) {
                    scene_.setBackgroundScreenSource(static_cast<core::ScreenTextureSource>(source));
                    refreshPreviewBackground();
                    refreshPreviewScene();
                }
                ImGui::EndTable();
            }
            bool enabled = background.enabled;
            if (ImGui::Checkbox("Enabled", &enabled)) {
                scene_.setBackgroundEnabled(enabled);
                refreshPreviewScene();
            }
        }
        ImGui::End();
        return;
    }

    ImGui::Text("Model: %s", model->displayName.c_str());
    ImGui::TextDisabled("PMX  ·  %zu bones  ·  %zu morphs", model->model->bones.size(), model->model->morphs.size());
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Model", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool visible = model->visible;
        if (ImGui::Checkbox("Visible", &visible)) {
            scene_.setModelVisible(model->id, visible);
            refreshAnimatedMesh(true);
        }
        int clones = static_cast<int>(model->cloneCount);
        if (ImGui::DragInt("Clone count", &clones, 1.0F, 1, 16)) {
            scene_.setCloneCount(model->id, static_cast<std::uint32_t>(clones));
            refreshAnimatedMesh(true);
        }
        ImGui::TextDisabled("Source: %s", model->sourcePath.filename().string().c_str());
        ImGui::TextDisabled("%s", model->model->metadata.comment.c_str());
    }
    if (ImGui::CollapsingHeader("Bone / Morph", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto& bones = model->model->bones;
        if (!bones.empty()) {
            selectedBone_ = std::clamp(selectedBone_, 0, static_cast<int>(bones.size() - 1));
            if (ImGui::BeginCombo("Bone", bones[static_cast<std::size_t>(selectedBone_)].name.c_str())) {
                for (std::size_t index = 0; index < bones.size(); ++index)
                    if (ImGui::Selectable(bones[index].name.c_str(), selectedBone_ == static_cast<int>(index)))
                        selectedBone_ = static_cast<int>(index);
                ImGui::EndCombo();
            }
            ImGui::DragFloat3("Position", editedBoneTranslation_.data(), 0.01F);
            ImGui::DragFloat4("Rotation", editedBoneRotation_.data(), 0.01F, -1.0F, 1.0F);
            ImGui::Checkbox("Physics", &editedBonePhysics_);
            if (ImGui::Button("Register Bone Key")) {
                const auto before = model->motion ? *model->motion : core::VmdMotion{};
                const auto outputModelName = before.modelName.empty() ? model->displayName : before.modelName;
                auto document = core::toMotionDocument(before);
                const auto frame = static_cast<std::uint32_t>(std::max(animationFrame_, 0.0F));
                const auto& name = bones[static_cast<std::size_t>(selectedBone_)].name;
                std::erase_if(document.bones, [&](const auto& key) { return key.frame == frame && key.name == name; });
                document.bones.push_back(
                    {name, frame, editedBoneTranslation_, editedBoneRotation_, {}, editedBonePhysics_});
                core::MotionEditor::normalize(document);
                history_.execute(scene_,
                                 std::make_unique<core::EditMotionCommand>(
                                     model->id, false, before, core::toVmdMotion(std::move(document), outputModelName),
                                     "Register bone key"));
                refreshAnimatedMesh(false);
                refreshPreviewScene();
            }
        }
        const auto& morphs = model->model->morphs;
        if (!morphs.empty()) {
            selectedMorph_ = std::clamp(selectedMorph_, 0, static_cast<int>(morphs.size() - 1));
            if (ImGui::BeginCombo("Morph", morphs[static_cast<std::size_t>(selectedMorph_)].name.c_str())) {
                for (std::size_t index = 0; index < morphs.size(); ++index)
                    if (ImGui::Selectable(morphs[index].name.c_str(), selectedMorph_ == static_cast<int>(index)))
                        selectedMorph_ = static_cast<int>(index);
                ImGui::EndCombo();
            }
            ImGui::SliderFloat("Weight", &editedMorphWeight_, 0.0F, 1.0F);
            if (ImGui::Button("Register Morph Key")) {
                const auto before = model->motion ? *model->motion : core::VmdMotion{};
                const auto outputModelName = before.modelName.empty() ? model->displayName : before.modelName;
                auto document = core::toMotionDocument(before);
                const auto frame = static_cast<std::uint32_t>(std::max(animationFrame_, 0.0F));
                const auto& name = morphs[static_cast<std::size_t>(selectedMorph_)].name;
                std::erase_if(document.morphs, [&](const auto& key) { return key.frame == frame && key.name == name; });
                document.morphs.push_back({name, frame, editedMorphWeight_});
                core::MotionEditor::normalize(document);
                history_.execute(scene_,
                                 std::make_unique<core::EditMotionCommand>(
                                     model->id, false, before, core::toVmdMotion(std::move(document), outputModelName),
                                     "Register morph key"));
                refreshAnimatedMesh(false);
                refreshPreviewScene();
            }
        }
    }
    if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (model->model->materials.empty()) {
            ImGui::TextDisabled("No materials");
        } else {
            int material = uiState_.selectedMaterial >= 0 &&
                                   uiState_.selectedMaterial < static_cast<std::int32_t>(model->model->materials.size())
                               ? uiState_.selectedMaterial
                               : 0;
            if (ImGui::BeginCombo("Material",
                                  model->model->materials[static_cast<std::size_t>(material)].name.c_str())) {
                for (std::size_t index = 0; index < model->model->materials.size(); ++index)
                    if (ImGui::Selectable(model->model->materials[index].name.c_str(),
                                          material == static_cast<int>(index))) {
                        uiState_.selectedMaterial = static_cast<std::int32_t>(index);
                        material = static_cast<int>(index);
                    }
                ImGui::EndCombo();
            }
            const auto& selectedMaterial = model->model->materials[static_cast<std::size_t>(material)];
            ImGui::Text("Diffuse %.2f  Specular %.2f  Edge %.3f", selectedMaterial.diffuse[0],
                        selectedMaterial.specular[0], selectedMaterial.edgeSize);
            ImGui::TextWrapped("Base: %s",
                               selectedMaterial.textureIndex >= 0 &&
                                       static_cast<std::size_t>(selectedMaterial.textureIndex) <
                                           model->model->textures.size()
                                   ? model->model->textures[static_cast<std::size_t>(selectedMaterial.textureIndex)]
                                         .filename()
                                         .string()
                                         .c_str()
                                   : "none");
            if (ImGui::Checkbox("Enable PMX outlines (preview)", &previewOutlineEnabled_))
                refreshPreviewScene();
        }
    }
    if (uiState_.workspace == ui::Workspace::debug && ImGui::CollapsingHeader("Evaluation Order (experimental)")) {
        ImGui::TextDisabled("Not applied by the renderer or saved in projects.");
        ImGui::BeginDisabled();
        ImGui::DragInt("Motion", &model->order.motion, 1.0F, 0, 1024);
        ImGui::DragInt("Deform", &model->order.deform, 1.0F, 0, 1024);
        ImGui::DragInt("Postprocess", &model->order.postprocess, 1.0F, 0, 1024);
        ImGui::DragInt("Raster", &model->order.raster, 1.0F, 0, 1024);
        ImGui::EndDisabled();
    }
    ImGui::End();
#endif
}

void Application::buildSaveAsDialog() {
#if DAYO_HAS_IMGUI
    if (uiState_.saveAsOpen) {
        ImGui::OpenPopup("Save Project As");
        uiState_.saveAsOpen = false;
    }
    if (!ImGui::BeginPopupModal("Save Project As", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextUnformatted("Choose a Dayo project destination.");
    ImGui::InputText("Path", projectDestination_.data(), projectDestination_.size());
    if (ImGui::Button("Save")) {
        saveProjectAsNow();
        if (projectSaveStatus_ == "Project saved")
            ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        ImGui::CloseCurrentPopup();
    if (!projectSaveStatus_.empty())
        ImGui::TextWrapped("%s", projectSaveStatus_.c_str());
    ImGui::EndPopup();
#endif
}

void Application::restoreVideoExportState() {
#if DAYO_HAS_IMGUI
    if (!videoExportRestorePending_)
        return;
    const float restoreFrame = std::max(videoExportRestoreFrame_, 0.0F);
    if (!advanceDeterministicFrameEvaluation(restoreFrame, videoRestoreNextFrame_))
        return;
    animationFrame_ = videoExportRestoreFrame_;
    scene_.setFrame(animationFrame_);
    mediaSeconds_ = videoExportRestoreMediaSeconds_;
    playing_ = videoExportRestorePlaying_;
    manualCamera_ = videoExportRestoreManualCamera_;
    if (videoMode_) {
        uploadedVideoFrame_ = -1;
        refreshVideoFrame();
    }
    if (videoExportRestoreAudioActive_ && !loadedAudio_.samples.empty()) {
        const auto audioStart = std::max(0.0, videoExportRestoreMediaSeconds_ + audioOffsetSeconds_);
        audioPlayer_.play(loadedAudio_, audioStart);
        audioPlayer_.setVolume(audioVolume_);
        audioPlayer_.setPaused(!playing_);
    } else {
        audioPlayer_.stop();
    }
    refreshPreviewScene();
    videoExportRestorePending_ = false;
    activeVideoExport_.reset();
#endif
}

void Application::saveProjectNow() {
#if DAYO_HAS_IMGUI
    if (!currentProjectPath_) {
        uiState_.saveAsOpen = true;
        projectSaveStatus_ = "Project has no path; choose a destination.";
        return;
    }
    try {
        core::saveProject(*currentProjectPath_, currentProject());
        projectSaveStatus_ = "Project saved";
    } catch (const std::exception& error) {
        projectSaveStatus_ = error.what();
    }
#endif
}

void Application::saveProjectAsNow() {
#if DAYO_HAS_IMGUI
    try {
        if (projectDestination_[0] == '\0')
            throw std::invalid_argument("project destination is empty");
        const auto destination = std::filesystem::absolute(projectDestination_.data()).lexically_normal();
        core::saveProject(destination, currentProject());
        currentProjectPath_ = destination;
        const auto text = currentProjectPath_->string();
        const auto count = std::min(text.size(), projectDestination_.size() - 1U);
        std::copy_n(text.data(), count, projectDestination_.data());
        projectDestination_[count] = '\0';
        projectSaveStatus_ = "Project saved";
    } catch (const std::exception& error) {
        projectSaveStatus_ = error.what();
    }
#endif
}

void Application::startImageSequenceExport() {
#if DAYO_HAS_IMGUI
    bool stateMutationStarted = false;
    try {
        if (device_ == nullptr)
            throw std::logic_error("image sequence export has no graphics device");
        if (sequenceOutput_.lastFrame < sequenceOutput_.firstFrame)
            throw std::invalid_argument("last frame precedes first frame");
        sequenceOutput_.directory = sequenceOutputDirectory_.data();
        core::OutputQueue output(sequenceOutput_);
        imageSequenceRestoreFrame_ = scene_.timeline().frame;
        imageSequenceRestoreMediaSeconds_ = mediaSeconds_;
        imageSequenceRestorePlaying_ = playing_;
        imageSequenceRestoreManualCamera_ = manualCamera_;
        imageSequenceNextFrame_ = sequenceOutput_.firstFrame;
        imageSequenceSampleIndex_ = 0;
        imageSequenceSampleCount_ = std::max(sequenceOutput_.samples, std::uint32_t{1});
        imageSequencePreviousSampleFrame_ = static_cast<float>(sequenceOutput_.firstFrame);
        imageSequenceFramesFinished_ = false;
        imageSequencePreRollDone_ = sequenceOutput_.firstFrame == 0U;
        imageSequencePreRollFrame_ = imageSequencePreRollDone_ ? 0U : 1U;
        imageSequenceRestoring_ = false;
        imageSequenceCancelRequested_ = false;
        imageSequenceCompletionStatus_.clear();
        imageSequenceImage_ = {};
        imageSequenceSum_.clear();
        stateMutationStarted = true;
        resetPhysicsSimulation();
        evaluateExportFrame(0.0F, 0.0F, true);
        imageSequenceOutput_.emplace(std::move(output));
        imageSequenceExportRunning_ = true;
        playing_ = false;
        if (audioPlayer_.active())
            audioPlayer_.setPaused(true);
        sequenceOutputStatus_ = "Rendering image sequence...";
    } catch (const std::exception& error) {
        imageSequenceOutput_.reset();
        imageSequenceExportRunning_ = false;
        imageSequenceRestoring_ = false;
        if (stateMutationStarted) {
            try {
                restoreImageSequenceState();
            } catch (const std::exception& restoreError) {
                sequenceOutputStatus_ = std::string(error.what()) + "; restore error: " + restoreError.what();
                return;
            }
        }
        sequenceOutputStatus_ = error.what();
    }
#endif
}

void Application::restoreImageSequenceState() {
#if DAYO_HAS_IMGUI
    scene_.setFrame(imageSequenceRestoreFrame_);
    animationFrame_ = imageSequenceRestoreFrame_;
    mediaSeconds_ = imageSequenceRestoreMediaSeconds_;
    playing_ = imageSequenceRestorePlaying_;
    manualCamera_ = imageSequenceRestoreManualCamera_;
    if (audioPlayer_.active())
        audioPlayer_.setPaused(!playing_);
    refreshAnimatedMesh(false);
    if (videoMode_) {
        uploadedVideoFrame_ = -1;
        refreshVideoFrame();
    }
    refreshPreviewScene();
#endif
}

void Application::finishImageSequenceExport(std::string status) {
#if DAYO_HAS_IMGUI
    try {
        if (imageSequenceOutput_) {
            imageSequenceOutput_->requestClose();
            if (!imageSequenceOutput_->finished()) {
                imageSequenceCompletionStatus_ = std::move(status);
                imageSequenceFramesFinished_ = true;
                return;
            }
            imageSequenceOutput_->rethrowIfFailed();
        }
        imageSequenceCompletionStatus_ = std::move(status);
    } catch (const std::exception& error) {
        imageSequenceCompletionStatus_ = error.what();
    }
    imageSequenceOutput_.reset();
    imageSequenceCancelRequested_ = false;
    imageSequenceFramesFinished_ = false;
    imageSequenceImage_ = {};
    imageSequenceSum_.clear();
    imageSequenceRestoring_ = true;
    imageSequenceRestoreNextFrame_ = 1U;
    resetPhysicsSimulation();
    evaluateExportFrame(0.0F, 0.0F, true);
    sequenceOutputStatus_ = "Restoring preview...";
#else
    static_cast<void>(status);
#endif
}

void Application::advanceImageSequenceExport() {
#if DAYO_HAS_IMGUI
    if (!imageSequenceExportRunning_)
        return;
    if (imageSequenceRestoring_) {
        if (!advanceDeterministicFrameEvaluation(imageSequenceRestoreFrame_, imageSequenceRestoreNextFrame_))
            return;
        imageSequenceRestoring_ = false;
        imageSequenceExportRunning_ = false;
        sequenceOutputStatus_ = std::move(imageSequenceCompletionStatus_);
        restoreImageSequenceState();
        return;
    }
    if (imageSequenceCancelRequested_) {
        finishImageSequenceExport("Sequence export cancelled");
        return;
    }
    if (imageSequenceFramesFinished_) {
        finishImageSequenceExport(imageSequenceCompletionStatus_.empty() ? "Sequence rendered"
                                                                         : imageSequenceCompletionStatus_);
        return;
    }
    if (!imageSequencePreRollDone_) {
        imageSequencePreRollDone_ = advanceDeterministicFrameEvaluation(static_cast<float>(sequenceOutput_.firstFrame),
                                                                        imageSequencePreRollFrame_);
        if (imageSequencePreRollDone_) {
            imageSequencePreviousSampleFrame_ = static_cast<float>(sequenceOutput_.firstFrame);
        }
        return;
    }
    try {
        if (!imageSequenceOutput_->canAcceptFrame())
            return;
        const auto frame = imageSequenceNextFrame_;
        const float offset = sequenceOutput_.motionBlur ? static_cast<float>(imageSequenceSampleIndex_) /
                                                              static_cast<float>(imageSequenceSampleCount_)
                                                        : 0.0F;
        const float sampleFrame = static_cast<float>(frame) + offset;
        const float physicsDelta = std::max(sampleFrame - imageSequencePreviousSampleFrame_, 0.0F) /
                                   static_cast<float>(sceneTimelineFps(scene_));
        evaluateExportFrame(sampleFrame, physicsDelta);
        imageSequencePreviousSampleFrame_ = sampleFrame;

        auto rendered = device_->renderToImage({sequenceWidth_, sequenceHeight_});
        if (imageSequenceSampleIndex_ == 0U) {
            imageSequenceImage_ = std::move(rendered);
            imageSequenceSum_.assign(imageSequenceImage_.pixels.size(), 0U);
            for (std::size_t index = 0; index < imageSequenceImage_.pixels.size(); ++index)
                imageSequenceSum_[index] += imageSequenceImage_.pixels[index];
        } else if (rendered.width != imageSequenceImage_.width || rendered.height != imageSequenceImage_.height ||
                   rendered.pixels.size() != imageSequenceImage_.pixels.size()) {
            throw std::runtime_error("image sequence samples have inconsistent dimensions");
        } else {
            for (std::size_t index = 0; index < imageSequenceImage_.pixels.size(); ++index)
                imageSequenceSum_[index] += rendered.pixels[index];
        }
        ++imageSequenceSampleIndex_;
        if (imageSequenceSampleIndex_ < imageSequenceSampleCount_)
            return;

        for (std::size_t index = 0; index < imageSequenceImage_.pixels.size(); ++index)
            imageSequenceImage_.pixels[index] =
                static_cast<std::uint8_t>(imageSequenceSum_[index] / imageSequenceSampleCount_);
        if (!imageSequenceOutput_->tryPush(frame, std::move(imageSequenceImage_)))
            throw std::runtime_error("image output queue unexpectedly full");
        imageSequenceSum_.clear();
        imageSequenceSampleIndex_ = 0;
        if (frame == sequenceOutput_.lastFrame)
            imageSequenceFramesFinished_ = true;
        else
            ++imageSequenceNextFrame_;
    } catch (const std::exception& error) {
        finishImageSequenceExport(error.what());
    }
#endif
}

void Application::buildImageSequenceExportUi() {
#if DAYO_HAS_IMGUI
    if (uiState_.imageSequenceExportOpen) {
        ImGui::OpenPopup("Export Image Sequence");
        uiState_.imageSequenceExportOpen = false;
    }
    if (!ImGui::BeginPopupModal("Export Image Sequence", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    if (imageSequenceExportRunning_) {
        if (imageSequenceRestoring_) {
            ImGui::TextUnformatted("Restoring preview state...");
        } else {
            const auto total = static_cast<std::uint64_t>(sequenceOutput_.lastFrame) -
                               static_cast<std::uint64_t>(sequenceOutput_.firstFrame) + 1U;
            const auto completed =
                imageSequencePreRollDone_
                    ? (imageSequenceFramesFinished_ ? total
                                                    : static_cast<std::uint64_t>(imageSequenceNextFrame_) -
                                                          static_cast<std::uint64_t>(sequenceOutput_.firstFrame))
                    : 0U;
            ImGui::Text("Rendering %u / %u",
                        imageSequenceFramesFinished_ ? sequenceOutput_.lastFrame : imageSequenceNextFrame_,
                        sequenceOutput_.lastFrame);
            ImGui::ProgressBar(total == 0U ? 0.0F : static_cast<float>(completed) / static_cast<float>(total),
                               {-1.0F, 0.0F});
            ImGui::Text("Current sample: %u / %u", imageSequenceSampleIndex_ + 1U, imageSequenceSampleCount_);
            if (ImGui::Button("Cancel"))
                imageSequenceCancelRequested_ = true;
        }
    } else {
        ImGui::InputText("Directory", sequenceOutputDirectory_.data(), sequenceOutputDirectory_.size());
        int first = static_cast<int>(sequenceOutput_.firstFrame);
        int last = static_cast<int>(sequenceOutput_.lastFrame);
        int samples = static_cast<int>(sequenceOutput_.samples);
        if (ImGui::InputInt("First frame", &first))
            sequenceOutput_.firstFrame = static_cast<std::uint32_t>(std::max(first, 0));
        if (ImGui::InputInt("Last frame", &last))
            sequenceOutput_.lastFrame = static_cast<std::uint32_t>(std::max(last, 0));
        if (ImGui::InputInt("Samples", &samples))
            sequenceOutput_.samples = static_cast<std::uint32_t>(std::clamp(samples, 1, 4096));
        int width = static_cast<int>(sequenceWidth_);
        int height = static_cast<int>(sequenceHeight_);
        if (ImGui::InputInt("Width", &width)) {
            sequenceWidth_ = static_cast<std::uint32_t>(std::max(width, 1));
            sequencePreset_ = 0;
        }
        if (ImGui::InputInt("Height", &height)) {
            sequenceHeight_ = static_cast<std::uint32_t>(std::max(height, 1));
            sequencePreset_ = 0;
        }
        if (ImGui::Combo("Preset", &sequencePreset_,
                         "Custom\0"
                         "720p\0"
                         "1080p\0"
                         "1440p\0"
                         "4K\0")) {
            constexpr std::array<std::array<std::uint32_t, 2>, 5> presets{{
                {0, 0},
                {1280, 720},
                {1920, 1080},
                {2560, 1440},
                {3840, 2160},
            }};
            const auto selected = presets[static_cast<std::size_t>(sequencePreset_)];
            if (selected[0] != 0U) {
                sequenceWidth_ = selected[0];
                sequenceHeight_ = selected[1];
            }
        }
        ImGui::Checkbox("Motion blur", &sequenceOutput_.motionBlur);
        ImGui::Checkbox("Overwrite existing frames", &sequenceOutput_.overwrite);
        if (sequenceOutput_.format == core::OutputFormat::exr)
            sequenceOutput_.format = core::OutputFormat::ppm;
        int format = std::clamp(static_cast<int>(sequenceOutput_.format), 0, 1);
        if (ImGui::Combo("Format", &format, "PPM\0PNG\0"))
            sequenceOutput_.format = static_cast<core::OutputFormat>(format);
        if (ImGui::Button("Render sequence"))
            startImageSequenceExport();
    }
    if (!sequenceOutputStatus_.empty())
        ImGui::TextWrapped("%s", sequenceOutputStatus_.c_str());
    ImGui::SameLine();
    if (!imageSequenceExportRunning_ && ImGui::Button("Close"))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
#endif
}

void Application::buildStatusBar() {
#if DAYO_HAS_IMGUI
    if (!uiState_.statusBarVisible)
        return;
    auto* viewport = ImGui::GetMainViewport();
    const float height = ImGui::GetFrameHeightWithSpacing();
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar;
    if (ImGui::BeginViewportSideBar("##status-bar", viewport, ImGuiDir_Down, height, flags)) {
        if (ImGui::BeginTable("##editor-status", 5, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 2.0F);
            ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthStretch, 1.0F);
            ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9.0F);
            ImGui::TableSetupColumn("FPS", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.0F);
            ImGui::TableSetupColumn("Project", ImGuiTableColumnFlags_WidthStretch, 1.5F);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(lastAsset_.c_str());
            if (ImGui::IsItemHovered() && !lastAsset_.empty())
                ImGui::SetTooltip("%s", lastAsset_.c_str());
            ImGui::TableSetColumnIndex(1);
            if (const auto* model = selectedModel())
                ImGui::TextUnformatted(model->displayName.c_str());
            else
                ImGui::TextDisabled("Scene");
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("Frame %.0f / %.0f", animationFrame_, scene_.timeline().duration);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.0f FPS", ImGui::GetIO().Framerate);
            ImGui::TableSetColumnIndex(4);
            const std::string projectText = currentProjectPath_ ? currentProjectPath_->string() : "Untitled project";
            ImGui::TextDisabled("%s", projectText.c_str());
            ImGui::EndTable();
        }
    }
    ImGui::End();
#endif
}

void Application::buildEditorUi() {
#if DAYO_HAS_IMGUI
    auto* model = selectedModel();
    const bool global = editGlobalMotion_;
    const auto* active = global ? scene_.cameraMotion() : (model != nullptr ? model->motion.get() : nullptr);
    const auto target = model != nullptr ? model->id : core::ModelId{};
    const auto cacheModelId = global ? core::ModelId{} : target;
    if (timelineTrackCache_.modelId != cacheModelId || timelineTrackCache_.motion != active ||
        timelineTrackCache_.globalMotion != global || timelineTrackCache_.motionRevision != scene_.motionRevision()) {
        selectedKeys_.clear();
        timelineTrackCache_ = {};
        timelineTrackCache_.motionRevision = scene_.motionRevision();
        timelineTrackCache_.modelId = cacheModelId;
        timelineTrackCache_.motion = active;
        timelineTrackCache_.globalMotion = global;
        timelineScrollY_ = 0.0F;
        if (active != nullptr) {
            const auto groupTracks = [](const auto& keys, std::vector<TimelineTrack>& tracks) {
                std::unordered_map<std::string, std::size_t> indices;
                indices.reserve(keys.size());
                for (const auto& key : keys) {
                    const auto [iterator, inserted] = indices.emplace(key.name, tracks.size());
                    if (inserted)
                        tracks.push_back({key.name, {}});
                    tracks[iterator->second].frames.push_back(key.frame);
                }
                for (auto& track : tracks)
                    std::sort(track.frames.begin(), track.frames.end());
            };
            groupTracks(active->bones, timelineTrackCache_.bones);
            groupTracks(active->morphs, timelineTrackCache_.morphs);
            const auto cacheFrames = [](const auto& keys, std::vector<std::uint32_t>& frames) {
                frames.reserve(keys.size());
                for (const auto& key : keys)
                    frames.push_back(key.frame);
                std::sort(frames.begin(), frames.end());
            };
            cacheFrames(active->cameras, timelineTrackCache_.cameras);
            cacheFrames(active->lights, timelineTrackCache_.lights);
        }
    }
    const auto execute = [&](core::VmdMotion before, core::MotionDocument document, bool globalMotion,
                             std::string label) {
        const auto outputModelName =
            before.modelName.empty() && !globalMotion && model != nullptr ? model->displayName : before.modelName;
        auto after = core::toVmdMotion(std::move(document), outputModelName);
        history_.execute(scene_, std::make_unique<core::EditMotionCommand>(target, globalMotion, std::move(before),
                                                                           std::move(after), std::move(label)));
        selectedKeys_.clear();
        active = global ? scene_.cameraMotion() : (model != nullptr ? model->motion.get() : nullptr);
        refreshAnimatedMesh(false);
        refreshPreviewScene();
    };
    const bool debugWorkspace = uiState_.workspace == ui::Workspace::debug;

    if (uiState_.timelineVisible && ImGui::Begin(workspaceWindowName("Timeline", "timeline").c_str())) {
        uiState_.timelineFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (ImGui::Button("|<")) {
            animationFrame_ = 0.0F;
            scene_.setFrame(animationFrame_);
            refreshAnimatedMesh(false);
            refreshPreviewScene();
        }
        ImGui::SameLine();
        if (ImGui::Button("<")) {
            animationFrame_ = std::max(0.0F, animationFrame_ - 1.0F);
            scene_.setFrame(animationFrame_);
            refreshAnimatedMesh(false);
            refreshPreviewScene();
        }
        ImGui::SameLine();
        if (ImGui::Button(playing_ ? "Pause" : "Play")) {
            playing_ = !playing_;
            if (audioPlayer_.active())
                audioPlayer_.setPaused(!playing_);
        }
        ImGui::SameLine();
        if (ImGui::Button(">")) {
            animationFrame_ = std::min(scene_.timeline().duration, animationFrame_ + 1.0F);
            scene_.setFrame(animationFrame_);
            refreshAnimatedMesh(false);
            refreshPreviewScene();
        }
        ImGui::SameLine();
        if (ImGui::Button(">|")) {
            animationFrame_ = scene_.timeline().duration;
            scene_.setFrame(animationFrame_);
            refreshAnimatedMesh(false);
            refreshPreviewScene();
        }
        ImGui::SameLine();
        ImGui::Text("Frame %.0f / %.0f", animationFrame_, scene_.timeline().duration);
        ImGui::SameLine();
        ImGui::Checkbox("Loop", &repeat_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0F);
        ImGui::DragFloat("Speed", &playbackSpeed_, 0.01F, 0.1F, 4.0F, "%.2fx");
        if (!waveformPeaks_.empty())
            ImGui::PlotLines("Audio", waveformPeaks_.data(), static_cast<int>(waveformPeaks_.size()), 0, nullptr, 0.0F,
                             1.0F, {-1.0F, 44.0F});
        ImGui::Checkbox("Key List", &timelineKeyListVisible_);
        ImGui::SameLine();
        if (ImGui::Checkbox("Edit global camera/light motion", &editGlobalMotion_))
            selectedKeys_.clear();
        if (active == nullptr) {
            ImGui::TextUnformatted("Load a VMD/VMdayo motion to edit keyframes.");
        } else {
            if (timelineKeyListVisible_)
                ImGui::Text("Bone %zu  Morph %zu  Camera %zu  Light %zu  Shadow %zu  IK %zu", active->bones.size(),
                            active->morphs.size(), active->cameras.size(), active->lights.size(),
                            active->shadows.size(), active->ik.size());
            auto row = [&](core::MotionTrack track, std::size_t index, std::uint32_t frame, const std::string& name) {
                const core::MotionKeyRef key{track, index};
                const bool selected = std::find(selectedKeys_.begin(), selectedKeys_.end(), key) != selectedKeys_.end();
                const auto label = name + "  @ " + std::to_string(frame) + "##" +
                                   std::to_string(static_cast<int>(track)) + ":" + std::to_string(index);
                if (!ImGui::Selectable(label.c_str(), selected))
                    return;
                if (!ImGui::GetIO().KeyCtrl)
                    selectedKeys_.clear();
                const auto found = std::find(selectedKeys_.begin(), selectedKeys_.end(), key);
                if (found == selectedKeys_.end())
                    selectedKeys_.push_back(key);
                else
                    selectedKeys_.erase(found);
            };
            const float canvasHeight = std::max(ImGui::GetFrameHeight() * 3.0F, ImGui::GetContentRegionAvail().y);
            if (timelineKeyListVisible_) {
                if (ImGui::BeginChild("key-list", {0.0F, canvasHeight}, true)) {
                    const auto count = active->bones.size() + active->morphs.size() + active->cameras.size() +
                                       active->lights.size() + active->shadows.size() + active->ik.size();
                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(count));
                    while (clipper.Step()) {
                        for (int visible = clipper.DisplayStart; visible < clipper.DisplayEnd; ++visible) {
                            auto index = static_cast<std::size_t>(visible);
                            const auto visit = [&](const auto& keys, core::MotionTrack track, const char* label) {
                                if (index < keys.size()) {
                                    const auto& key = keys[index];
                                    if constexpr (requires { key.name; })
                                        row(track, index, key.frame, key.name);
                                    else
                                        row(track, index, key.frame, label);
                                    return true;
                                }
                                index -= keys.size();
                                return false;
                            };
                            if (visit(active->bones, core::MotionTrack::bone, "Bone") ||
                                visit(active->morphs, core::MotionTrack::morph, "Morph") ||
                                visit(active->cameras, core::MotionTrack::camera, "Camera") ||
                                visit(active->lights, core::MotionTrack::light, "Light") ||
                                visit(active->shadows, core::MotionTrack::shadow, "Self shadow"))
                                continue;
                            static_cast<void>(visit(active->ik, core::MotionTrack::ik, "IK / visibility"));
                        }
                    }
                }
                ImGui::EndChild();
            } else {
                if (ImGui::BeginChild("timeline-canvas", {0.0F, canvasHeight}, true,
                                      ImGuiWindowFlags_NoScrollWithMouse)) {
                    const auto canvasMin = ImGui::GetWindowPos();
                    const auto canvasSize = ImGui::GetWindowSize();
                    auto* drawList = ImGui::GetWindowDrawList();
                    const float left = canvasMin.x + ImGui::GetFontSize() * 8.0F;
                    const float top = canvasMin.y + ImGui::GetFrameHeight();
                    const float right = canvasMin.x + canvasSize.x - 8.0F;
                    const float bottom = canvasMin.y + canvasSize.y - 8.0F;
                    const float duration = std::max(scene_.timeline().duration, 1.0F);
                    float pixelsPerFrame = (right - left) / duration * timelineZoom_;
                    float maxPan = std::max(0.0F, duration * pixelsPerFrame - (right - left));
                    timelinePan_ = std::clamp(timelinePan_, 0.0F, maxPan);
                    const float rowHeight = 22.0F;
                    const int trackCount = global ? 2
                                                  : 2 + static_cast<int>(timelineTrackCache_.bones.size() +
                                                                         timelineTrackCache_.morphs.size());
                    const float contentHeight = 16.0F + static_cast<float>(trackCount) * rowHeight;
                    const float visibleHeight = std::max(0.0F, bottom - top);
                    const float maxScrollY = std::max(0.0F, contentHeight - visibleHeight);
                    timelineScrollY_ = std::clamp(timelineScrollY_, 0.0F, maxScrollY);
                    const auto frameX = [&](float frame) { return left + frame * pixelsPerFrame - timelinePan_; };
                    drawList->AddText({canvasMin.x + 8.0F, canvasMin.y + 5.0F}, ImGui::GetColorU32(ImGuiCol_Text),
                                      "Tracks");
                    for (int tick = 0; tick <= 10; ++tick) {
                        const float frame = duration * static_cast<float>(tick) / 10.0F;
                        const float x = frameX(frame);
                        if (x < left - 1.0F || x > right + 1.0F)
                            continue;
                        drawList->AddLine({x, top}, {x, bottom}, ImGui::GetColorU32(ImGuiCol_Border));
                        const auto label = std::to_string(static_cast<int>(frame));
                        drawList->AddText({x + 2.0F, canvasMin.y + 5.0F}, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                          label.c_str());
                    }
                    const auto trackY = [&](int trackRow) {
                        return top + 16.0F + static_cast<float>(trackRow) * rowHeight - timelineScrollY_;
                    };
                    const auto trackVisible = [&](int trackRow) {
                        const float y = trackY(trackRow);
                        return y >= top - ImGui::GetFontSize() && y <= bottom + ImGui::GetFontSize();
                    };
                    const auto drawDiamond = [&](float frame, int trackRow, ImU32 color) {
                        const float x = frameX(frame);
                        const float y = trackY(trackRow);
                        if (x < left - 8.0F || x > right + 8.0F || y < top - 8.0F || y > bottom + 8.0F)
                            return;
                        drawList->AddQuadFilled({x, y - 5.0F}, {x + 5.0F, y}, {x, y + 5.0F}, {x - 5.0F, y}, color);
                    };
                    const auto drawTrack = [&](const char* name, int trackRow) {
                        if (!trackVisible(trackRow))
                            return;
                        const float y = trackY(trackRow);
                        drawList->AddText({canvasMin.x + 8.0F, y - ImGui::GetFontSize() * 0.5F},
                                          ImGui::GetColorU32(ImGuiCol_Text), name);
                    };
                    drawTrack("Camera", 0);
                    drawTrack("Light", 1);
                    const auto drawFrames = [&](const std::vector<std::uint32_t>& frames, int trackRow, ImU32 color) {
                        const float firstFrame = (timelinePan_ - 8.0F) / pixelsPerFrame;
                        const float lastFrame = (right - left + timelinePan_ + 8.0F) / pixelsPerFrame;
                        const auto first = std::lower_bound(
                            frames.begin(), frames.end(), firstFrame,
                            [](std::uint32_t frame, float value) { return static_cast<float>(frame) < value; });
                        const auto last =
                            std::upper_bound(first, frames.end(), lastFrame, [](float value, std::uint32_t frame) {
                                return value < static_cast<float>(frame);
                            });
                        for (auto it = first; it != last; ++it)
                            drawDiamond(static_cast<float>(*it), trackRow, color);
                    };
                    if (global) {
                        if (trackVisible(0))
                            drawFrames(timelineTrackCache_.cameras, 0, ImGui::GetColorU32(ImGuiCol_CheckMark));
                        if (trackVisible(1))
                            drawFrames(timelineTrackCache_.lights, 1, ImGui::GetColorU32(ImGuiCol_CheckMark));
                    } else if (model != nullptr) {
                        int trackRow = 2;
                        for (const auto& track : timelineTrackCache_.bones) {
                            if (trackVisible(trackRow)) {
                                drawTrack(track.name.c_str(), trackRow);
                                drawFrames(track.frames, trackRow, ImGui::GetColorU32(ImGuiCol_SliderGrab));
                            }
                            ++trackRow;
                        }
                        for (const auto& track : timelineTrackCache_.morphs) {
                            if (trackVisible(trackRow)) {
                                drawTrack(track.name.c_str(), trackRow);
                                drawFrames(track.frames, trackRow, ImGui::GetColorU32(ImGuiCol_PlotLines));
                            }
                            ++trackRow;
                        }
                    }
                    const float currentX = frameX(animationFrame_);
                    if (currentX >= left && currentX <= right)
                        drawList->AddLine({currentX, top}, {currentX, bottom},
                                          ImGui::GetColorU32(ImGuiCol_PlotHistogram), 2.0F);
                    ImGui::SetCursorScreenPos({left, top});
                    ImGui::InvisibleButton("timeline-canvas-input", {right - left, bottom - top},
                                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
                    if (ImGui::IsItemHovered()) {
                        if (ImGui::GetIO().MouseWheel != 0.0F) {
                            if (ImGui::GetIO().KeyCtrl) {
                                const float frameUnderCursor =
                                    (ImGui::GetIO().MousePos.x - left + timelinePan_) / pixelsPerFrame;
                                timelineZoom_ =
                                    std::clamp(timelineZoom_ + ImGui::GetIO().MouseWheel * 0.1F, 0.5F, 8.0F);
                                pixelsPerFrame = (right - left) / duration * timelineZoom_;
                                maxPan = std::max(0.0F, duration * pixelsPerFrame - (right - left));
                                timelinePan_ =
                                    std::clamp(frameUnderCursor * pixelsPerFrame - (ImGui::GetIO().MousePos.x - left),
                                               0.0F, maxPan);
                            } else {
                                timelineScrollY_ = std::clamp(timelineScrollY_ - ImGui::GetIO().MouseWheel * rowHeight,
                                                              0.0F, maxScrollY);
                            }
                        }
                        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                            timelinePan_ = std::clamp(timelinePan_ - ImGui::GetIO().MouseDelta.x, 0.0F, maxPan);
                        }
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                            const float frame = (ImGui::GetIO().MousePos.x - left + timelinePan_) / pixelsPerFrame;
                            animationFrame_ = std::clamp(frame, 0.0F, duration);
                            scene_.setFrame(animationFrame_);
                            refreshAnimatedMesh(false);
                            refreshPreviewScene();
                        }
                    }
                }
                ImGui::EndChild();
            }
            const bool keyWindowFocused =
                ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput;
            const bool copyShortcut =
                keyWindowFocused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false);
            const bool cutShortcut =
                keyWindowFocused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false);
            const bool pasteShortcut =
                keyWindowFocused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false);
            const bool deleteShortcut = keyWindowFocused && ImGui::IsKeyPressed(ImGuiKey_Delete, false);
            if ((ImGui::Button("Copy") || copyShortcut) && !selectedKeys_.empty()) {
                motionClipboard_ = core::MotionEditor::copy(core::toMotionDocument(*active), selectedKeys_);
            }
            ImGui::SameLine();
            if ((ImGui::Button("Cut") || cutShortcut) && !selectedKeys_.empty()) {
                auto before = *active;
                auto document = core::toMotionDocument(before);
                motionClipboard_ = core::MotionEditor::copy(document, selectedKeys_);
                core::MotionEditor::erase(document, selectedKeys_);
                selectedKeys_.clear();
                execute(std::move(before), std::move(document), global, "Cut keys");
            }
            ImGui::SameLine();
            if ((ImGui::Button("Paste") || pasteShortcut) && !motionClipboard_.empty()) {
                auto before = *active;
                auto document = core::toMotionDocument(before);
                static_cast<void>(core::MotionEditor::paste(
                    document, motionClipboard_, static_cast<std::uint32_t>(std::max(animationFrame_, 0.0F))));
                selectedKeys_.clear();
                execute(std::move(before), std::move(document), global, "Paste keys");
            }
            ImGui::SameLine();
            if ((ImGui::Button("Delete") || deleteShortcut) && !selectedKeys_.empty()) {
                auto before = *active;
                auto document = core::toMotionDocument(before);
                core::MotionEditor::erase(document, selectedKeys_);
                selectedKeys_.clear();
                execute(std::move(before), std::move(document), global, "Delete keys");
            }
            int interpolation = static_cast<int>(active->interpolation);
            if (ImGui::Combo("Interpolation", &interpolation, "Linear\0VMD Bezier\0Catmull-Rom\0")) {
                auto before = *active;
                auto document = core::toMotionDocument(before);
                document.interpolation = static_cast<core::InterpolationMode>(interpolation);
                execute(std::move(before), std::move(document), global, "Set interpolation");
            }
            static std::array<char, 1024> exportPath{};
            ImGui::InputText("VMD destination", exportPath.data(), exportPath.size());
            if (ImGui::Button("Export VMD") && exportPath[0] != '\0') {
                try {
                    core::saveVmd(exportPath.data(), *active);
                    lastAsset_ = "Exported VMD";
                } catch (const std::exception& error) {
                    lastAsset_ = error.what();
                }
            }
        }
    }
    if (uiState_.timelineVisible)
        ImGui::End();

    if (debugWorkspace && ImGui::Begin(workspaceWindowName("Bone / Expression", "legacy-bone").c_str())) {
        if (model == nullptr || model->model == nullptr) {
            ImGui::TextUnformatted("Select a model to edit bones and morphs.");
        } else {
            const auto& bones = model->model->bones;
            if (!bones.empty()) {
                selectedBone_ = std::clamp(selectedBone_, 0, static_cast<int>(bones.size() - 1));
                if (ImGui::BeginCombo("Bone", bones[static_cast<std::size_t>(selectedBone_)].name.c_str())) {
                    for (std::size_t i = 0; i < bones.size(); ++i)
                        if (ImGui::Selectable(bones[i].name.c_str(), selectedBone_ == static_cast<int>(i)))
                            selectedBone_ = static_cast<int>(i);
                    ImGui::EndCombo();
                }
                ImGui::DragFloat3("Translation", editedBoneTranslation_.data(), 0.01F);
                ImGui::DragFloat4("Rotation quaternion", editedBoneRotation_.data(), 0.01F, -1.0F, 1.0F);
                ImGui::Checkbox("Bone physics", &editedBonePhysics_);
                if (ImGui::Button("Register bone")) {
                    const core::VmdMotion before = model->motion ? *model->motion : core::VmdMotion{};
                    auto document = core::toMotionDocument(before);
                    const auto frame = static_cast<std::uint32_t>(std::max(animationFrame_, 0.0F));
                    const auto& name = bones[static_cast<std::size_t>(selectedBone_)].name;
                    std::erase_if(document.bones,
                                  [&](const auto& key) { return key.frame == frame && key.name == name; });
                    document.bones.push_back(
                        {name, frame, editedBoneTranslation_, editedBoneRotation_, {}, editedBonePhysics_});
                    core::MotionEditor::normalize(document);
                    execute(before, std::move(document), false, "Register bone key");
                }
            }
            const auto& morphs = model->model->morphs;
            if (!morphs.empty()) {
                selectedMorph_ = std::clamp(selectedMorph_, 0, static_cast<int>(morphs.size() - 1));
                if (ImGui::BeginCombo("Morph", morphs[static_cast<std::size_t>(selectedMorph_)].name.c_str())) {
                    for (std::size_t i = 0; i < morphs.size(); ++i)
                        if (ImGui::Selectable(morphs[i].name.c_str(), selectedMorph_ == static_cast<int>(i)))
                            selectedMorph_ = static_cast<int>(i);
                    ImGui::EndCombo();
                }
                ImGui::SliderFloat("Weight", &editedMorphWeight_, 0.0F, 1.0F);
                if (ImGui::Button("Register morph")) {
                    const core::VmdMotion before = model->motion ? *model->motion : core::VmdMotion{};
                    auto document = core::toMotionDocument(before);
                    const auto frame = static_cast<std::uint32_t>(std::max(animationFrame_, 0.0F));
                    const auto& name = morphs[static_cast<std::size_t>(selectedMorph_)].name;
                    std::erase_if(document.morphs,
                                  [&](const auto& key) { return key.frame == frame && key.name == name; });
                    document.morphs.push_back({name, frame, editedMorphWeight_});
                    core::MotionEditor::normalize(document);
                    execute(before, std::move(document), false, "Register morph key");
                }
            }
        }
    }
    if (debugWorkspace)
        ImGui::End();

    const bool cameraPanelVisible = debugWorkspace || uiState_.workspace == ui::Workspace::camera;
    if (cameraPanelVisible &&
        ImGui::Begin(workspaceWindowName("Camera / Light / Self Shadow", "legacy-camera").c_str())) {
        ImGui::Checkbox("Realtime camera recording (Space registers)", &recordCamera_);
        ImGui::DragFloat3("Camera target", editedCamera_.position.data(), 0.01F);
        ImGui::DragFloat3("Camera rotation", editedCamera_.rotation.data(), 0.01F);
        ImGui::DragFloat("Camera distance", &editedCamera_.distance, 0.1F);
        int viewAngle = static_cast<int>(editedCamera_.viewAngle == 0 ? 30 : editedCamera_.viewAngle);
        if (ImGui::SliderInt("FoV", &viewAngle, 1, 179))
            editedCamera_.viewAngle = static_cast<float>(viewAngle);
        ImGui::Checkbox("Perspective", &editedCamera_.perspective);
        ImGui::InputInt("Camera parent model", &editedCamera_.parentModel);
        ImGui::InputInt("Camera parent bone", &editedCamera_.parentBone);
        ImGui::InputText("Camera parent bone name", cameraParentBoneName_.data(), cameraParentBoneName_.size());
        if (ImGui::Button("Register camera")) {
            core::VmdMotion before = scene_.cameraMotion() ? *scene_.cameraMotion() : core::VmdMotion{};
            auto document = core::toMotionDocument(before);
            editedCamera_.frame = static_cast<std::uint32_t>(std::max(animationFrame_, 0.0F));
            editedCamera_.parentBoneName = cameraParentBoneName_.data();
            std::erase_if(document.cameras, [&](const auto& key) { return key.frame == editedCamera_.frame; });
            document.cameras.push_back(editedCamera_);
            core::MotionEditor::normalize(document);
            execute(std::move(before), std::move(document), true, "Register camera key");
        }
        ImGui::ColorEdit3("Light color", editedLight_.color.data());
        ImGui::DragFloat3("Light direction", editedLight_.position.data(), 0.01F);
        if (ImGui::Button("Register light")) {
            core::VmdMotion before = scene_.cameraMotion() ? *scene_.cameraMotion() : core::VmdMotion{};
            auto document = core::toMotionDocument(before);
            editedLight_.frame = static_cast<std::uint32_t>(std::max(animationFrame_, 0.0F));
            std::erase_if(document.lights, [&](const auto& key) { return key.frame == editedLight_.frame; });
            document.lights.push_back(editedLight_);
            core::MotionEditor::normalize(document);
            execute(std::move(before), std::move(document), true, "Register light key");
        }
        int shadowMode = editedShadow_.mode;
        if (ImGui::Combo("Self shadow", &shadowMode, "None\0Mode 1\0Mode 2\0"))
            editedShadow_.mode = static_cast<std::uint8_t>(shadowMode);
        ImGui::DragFloat("Shadow distance", &editedShadow_.distance, 0.1F, 0.0F, 10'000.0F);
        if (ImGui::Button("Register self shadow")) {
            core::VmdMotion before = scene_.cameraMotion() ? *scene_.cameraMotion() : core::VmdMotion{};
            auto document = core::toMotionDocument(before);
            editedShadow_.frame = static_cast<std::uint32_t>(std::max(animationFrame_, 0.0F));
            std::erase_if(document.shadows, [&](const auto& key) { return key.frame == editedShadow_.frame; });
            document.shadows.push_back(editedShadow_);
            core::MotionEditor::normalize(document);
            execute(std::move(before), std::move(document), true, "Register self shadow key");
        }
    }
    if (cameraPanelVisible)
        ImGui::End();

    if (debugWorkspace && ImGui::Begin(workspaceWindowName("Project tools", "legacy-project").c_str())) {
        ImGui::InputText("Project file", projectDestination_.data(), projectDestination_.size());
        if (ImGui::Button("Save Dayo 1.30 project"))
            saveProjectAsNow();
        if (!projectSaveStatus_.empty())
            ImGui::TextWrapped("%s", projectSaveStatus_.c_str());
        ImGui::Separator();
        auto background = scene_.background();
        int source = static_cast<int>(background.screenSource);
        if (ImGui::Combo("Background source", &source, "Previous frame\0Video\0Image\0White\0")) {
            scene_.setBackgroundScreenSource(static_cast<core::ScreenTextureSource>(source));
            refreshPreviewBackground();
            refreshPreviewScene();
        }
        bool enabled = background.enabled;
        if (ImGui::Checkbox("Background enabled", &enabled)) {
            scene_.setBackgroundEnabled(enabled);
            refreshPreviewScene();
        }
        bool crop = background.crop == core::ScreenCropMode::crop4x3;
        if (ImGui::Checkbox("Crop 4:3", &crop)) {
            scene_.setBackgroundCrop(crop ? core::ScreenCropMode::crop4x3 : core::ScreenCropMode::none);
            refreshPreviewScene();
        }
        bool alpha = background.mode == core::BackgroundMode::alpha;
        if (ImGui::Checkbox("Alpha background", &alpha)) {
            scene_.setBackgroundMode(alpha ? core::BackgroundMode::alpha : core::BackgroundMode::opaque);
            refreshPreviewScene();
        }
        if (model != nullptr) {
            ImGui::SeparatorText("Model order");
            ImGui::DragInt("Motion order", &model->order.motion, 1.0F, 0, 1024);
            ImGui::DragInt("Deform order", &model->order.deform, 1.0F, 0, 1024);
            ImGui::DragInt("Postprocess order", &model->order.postprocess, 1.0F, 0, 1024);
            ImGui::DragInt("Raster order", &model->order.raster, 1.0F, 0, 1024);
            if (ImGui::TreeNode("Model description")) {
                ImGui::TextWrapped("%s", model->model->metadata.comment.c_str());
                ImGui::TextUnformatted(model->sourcePath.parent_path().string().c_str());
                if (ImGui::Button("Open model folder")) {
                    const auto url = "file://" + model->sourcePath.parent_path().generic_string();
                    if (!SDL_OpenURL(url.c_str()))
                        lastAsset_ = std::string("Open folder: ") + SDL_GetError();
                }
                ImGui::TreePop();
            }
            if (!model->materialSettings.empty() && ImGui::TreeNode("Material annotations")) {
                static int materialIndex = 0;
                materialIndex = std::clamp(materialIndex, 0, static_cast<int>(model->materialSettings.size() - 1));
                const auto& materials = model->model->materials;
                const char* preview = materials[static_cast<std::size_t>(materialIndex)].name.c_str();
                if (ImGui::BeginCombo("Material", preview)) {
                    for (std::size_t i = 0; i < materials.size(); ++i)
                        if (ImGui::Selectable(materials[i].name.c_str(), materialIndex == static_cast<int>(i)))
                            materialIndex = static_cast<int>(i);
                    ImGui::EndCombo();
                }
                static std::array<char, 1024> annotation{};
                ImGui::InputText("Annotation / MatDesc", annotation.data(), annotation.size());
                if (ImGui::Button("Apply material annotation")) {
                    model->materialSettings[static_cast<std::size_t>(materialIndex)].annotation = annotation.data();
                    scene_.markDirty(core::DirtyFlag::material | core::DirtyFlag::effect);
                }
                ImGui::TreePop();
            }
            if (scene_.models().size() > 1 && ImGui::TreeNode("External parent")) {
                static int parentIndex = 0;
                static std::array<char, 256> parentBone{};
                static std::array<char, 256> childBone{};
                parentIndex = std::clamp(parentIndex, 0, static_cast<int>(scene_.models().size() - 1));
                if (ImGui::BeginCombo("Parent model",
                                      scene_.models()[static_cast<std::size_t>(parentIndex)].displayName.c_str())) {
                    for (std::size_t i = 0; i < scene_.models().size(); ++i) {
                        if (ImGui::Selectable(scene_.models()[i].displayName.c_str(),
                                              parentIndex == static_cast<int>(i)))
                            parentIndex = static_cast<int>(i);
                    }
                    ImGui::EndCombo();
                }
                ImGui::InputText("Parent bone", parentBone.data(), parentBone.size());
                ImGui::InputText("Child bone", childBone.data(), childBone.size());
                if (ImGui::Button("Add external parent")) {
                    std::string error;
                    const auto& parent = scene_.models()[static_cast<std::size_t>(parentIndex)];
                    if (!scene_.addExternalParent({parent.id, parentBone.data(), model->id, childBone.data()},
                                                  &error)) {
                        lastAsset_ = "External parent: " + error;
                    } else {
                        const core::VmdMotion before = model->motion ? *model->motion : core::VmdMotion{};
                        auto document = core::toMotionDocument(before);
                        const auto frame = static_cast<std::uint32_t>(std::max(animationFrame_, 0.0F));
                        document.externalParents.push_back(
                            {frame, static_cast<std::int32_t>(parent.id), parentBone.data(), childBone.data()});
                        core::MotionEditor::normalize(document);
                        execute(before, std::move(document), false, "Register external parent key");
                    }
                }
                ImGui::TreePop();
            }
        }
        if (ImGui::Button("Copy borrowed-list")) {
            std::string credits;
            std::vector<std::filesystem::path> scanned;
            for (const auto& asset : projectAssets_) {
                credits += asset.kind + ": " + asset.path.string() + "\n";
                const auto directory = asset.path.parent_path();
                if (std::find(scanned.begin(), scanned.end(), directory) != scanned.end())
                    continue;
                scanned.push_back(directory);
                std::error_code directoryError;
                for (std::filesystem::directory_iterator iterator(directory, directoryError), end;
                     !directoryError && iterator != end; iterator.increment(directoryError)) {
                    auto extension = iterator->path().extension().string();
                    std::ranges::transform(extension, extension.begin(),
                                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
                    if (extension != ".url")
                        continue;
                    std::ifstream input(iterator->path());
                    std::string line;
                    while (std::getline(input, line))
                        if (line.starts_with("URL=")) {
                            credits += "url: " + line.substr(4) + "\n";
                            break;
                        }
                }
            }
            if (model != nullptr && !model->model->metadata.comment.empty())
                credits += model->model->metadata.comment + "\n";
            ImGui::SetClipboardText(credits.c_str());
        }
        if (ImGui::TreeNode("Shortcuts")) {
            ImGui::TextUnformatted("Space: Play / Pause\nCtrl+Z: Undo\nCtrl+Y: Redo\nCtrl+C/X/V: Copy/Cut/Paste "
                                   "keys\nDelete: Delete selected keys");
            ImGui::TreePop();
        }
    }
    if (debugWorkspace)
        ImGui::End();

    if (uiState_.materialDebugVisible &&
        ImGui::Begin(workspaceWindowName("Preview Material Inspector", "material-debug").c_str())) {
        bool previewChanged = false;
        previewChanged |= ImGui::Checkbox("Optional PMX outline", &previewOutlineEnabled_);
        if (model != nullptr && !model->model->materials.empty()) {
            const auto materialBase = static_cast<std::int32_t>(scene_.materialBase(model->id));
            const auto localCount = static_cast<int>(model->model->materials.size());
            int localMaterial =
                previewDebugMaterial_ >= materialBase && previewDebugMaterial_ < materialBase + localCount
                    ? previewDebugMaterial_ - materialBase
                    : 0;
            const auto& selectedMaterial = model->model->materials[static_cast<std::size_t>(localMaterial)];
            if (ImGui::BeginCombo("Material", selectedMaterial.name.c_str())) {
                for (std::size_t index = 0; index < model->model->materials.size(); ++index) {
                    if (ImGui::Selectable(model->model->materials[index].name.c_str(),
                                          localMaterial == static_cast<int>(index))) {
                        localMaterial = static_cast<int>(index);
                        previewDebugMaterial_ = materialBase + localMaterial;
                        previewChanged = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::Button("Show all materials")) {
                previewDebugMaterial_ = -1;
                previewChanged = true;
            }
            const auto& material = model->model->materials[static_cast<std::size_t>(localMaterial)];
            const auto textureName = [&](std::int32_t index) {
                if (index < 0 || static_cast<std::size_t>(index) >= model->model->textures.size())
                    return std::string("none");
                return model->model->textures[static_cast<std::size_t>(index)].filename().string();
            };
            ImGui::Text("Base: %s", textureName(material.textureIndex).c_str());
            ImGui::Text("Sphere: %s / mode %u", textureName(material.sphereTextureIndex).c_str(),
                        static_cast<unsigned>(material.sphereMode));
            if (material.toonMode == 0)
                ImGui::Text("Toon: %s / individual", textureName(material.toonTextureIndex).c_str());
            else
                ImGui::Text("Toon: shared toon%02d.bmp", material.toonTextureIndex + 1);
            ImGui::Text("Diffuse alpha: %.3f  edge size: %.5f", material.diffuse[3], material.edgeSize);
            ImGui::Text("Double-sided: %s  indices: %u", (material.drawFlags & 0x01U) != 0 ? "yes" : "no",
                        material.indexCount);
            previewChanged |= ImGui::CheckboxFlags("Disable base texture", &previewDebugFlags_,
                                                   graphics::previewDebugDisableBaseTexture);
            previewChanged |=
                ImGui::CheckboxFlags("Disable sphere", &previewDebugFlags_, graphics::previewDebugDisableSphere);
            previewChanged |=
                ImGui::CheckboxFlags("Disable toon", &previewDebugFlags_, graphics::previewDebugDisableToon);
            previewChanged |= ImGui::CheckboxFlags("Show normals", &previewDebugFlags_, graphics::previewDebugNormals);
            previewChanged |= ImGui::CheckboxFlags("Show UV", &previewDebugFlags_, graphics::previewDebugUv);
            previewChanged |=
                ImGui::CheckboxFlags("Disable sidedness", &previewDebugFlags_, graphics::previewDebugDisableSidedness);
        } else {
            ImGui::TextUnformatted("No PMX model selected");
        }
        if (previewChanged)
            refreshPreviewScene();
    }
    if (uiState_.materialDebugVisible)
        ImGui::End();

    if (uiState_.fxDebugVisible && ImGui::Begin(workspaceWindowName("FX Debug", "fx-debug").c_str())) {
        if (const auto* effect = scene_.effect()) {
            const auto compiled = core::compileEffectGraph(*effect);
            ImGui::Text("Resources: %zu", effect->textures.size());
            for (const auto& texture : effect->textures)
                ImGui::BulletText("%s (%s)", texture.name.c_str(), texture.format.c_str());
            ImGui::SeparatorText("Passes");
            for (const auto& pass : compiled.passes) {
                if (ImGui::TreeNode(pass.name.c_str())) {
                    ImGui::Text("Type: %s", core::toString(pass.type));
                    for (const auto& barrier : pass.barriers)
                        ImGui::BulletText("Barrier: %s", barrier.c_str());
                    for (const auto& resource : pass.resources)
                        ImGui::BulletText("%s %s", resource.write ? "Write" : "Read", resource.resource.c_str());
                    ImGui::TreePop();
                }
            }
        } else
            ImGui::TextUnformatted("Load an .fxdayo file to inspect resources and passes.");
    }
    if (uiState_.fxDebugVisible)
        ImGui::End();
#endif
}

void Application::setAudioExportDestinationForSource(const std::filesystem::path& source) {
    audioSource_ = std::filesystem::absolute(source);
    if (audioDestination_[0] == '\0') {
        const auto destination = audioSource_.parent_path() / (audioSource_.stem().string() + ".m4a");
        const auto text = destination.string();
        const auto count = std::min(text.size(), audioDestination_.size() - 1U);
        std::copy_n(text.data(), count, audioDestination_.data());
        audioDestination_[count] = '\0';
    }
}

void Application::buildAudioExportUi() {
#if DAYO_HAS_IMGUI
    if (uiState_.audioExportOpen) {
        ImGui::OpenPopup("Export Audio");
        uiState_.audioExportOpen = false;
    }
    if (ImGui::BeginPopupModal("Export Audio", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!DAYO_HAS_MEDIA) {
            ImGui::TextUnformatted("FFmpeg support is not available in this build.");
        } else {
            ImGui::TextUnformatted("Format");
            ImGui::SameLine(120.0F);
            ImGui::TextUnformatted("M4A / AAC");
            ImGui::TextUnformatted("Source");
            ImGui::SameLine(120.0F);
            if (audioSource_.empty())
                ImGui::TextUnformatted("Load an audio or video asset first");
            else
                ImGui::TextWrapped("%s", audioSource_.string().c_str());

            ImGui::SliderInt("Bitrate (kbps)", &audioBitrateKbps_, 64, 512);
            ImGui::TextUnformatted("Range");
            ImGui::SameLine(120.0F);
            ImGui::RadioButton("Full", &audioRangeMode_, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Selection", &audioRangeMode_, 1);
            if (audioRangeMode_ == 1) {
                ImGui::DragFloat("From (seconds)", &audioFromSeconds_, 0.1F, 0.0F, audioToSeconds_);
                ImGui::DragFloat("To (seconds)", &audioToSeconds_, 0.1F, audioFromSeconds_, 0.0F);
            }
            ImGui::InputText("Output", audioDestination_.data(), audioDestination_.size());
            ImGui::Checkbox("Overwrite existing file", &audioOverwrite_);
            ImGui::Separator();

            if (audioExportJob_.running()) {
                const float progress = audioExportJob_.progress();
                ImGui::ProgressBar(progress, {-1.0F, 0.0F});
                ImGui::Text("%.1f%%", static_cast<double>(progress) * 100.0);
                if (audioExportJob_.totalSeconds() > 0.0) {
                    ImGui::SameLine();
                    ImGui::Text("%.2f / %.2f s", audioExportJob_.processedSeconds(), audioExportJob_.totalSeconds());
                }
                if (ImGui::Button("Cancel"))
                    audioExportJob_.cancel();
            } else {
                const auto error = audioExportJob_.error();
                if (error)
                    ImGui::TextWrapped("Export error: %s", error->c_str());
                const auto result = audioExportJob_.result();
                if (result)
                    ImGui::TextWrapped("Exported: %s", result->output.string().c_str());
                if (ImGui::Button("Export")) {
                    core::AudioExportRequest request;
                    request.source = audioSource_;
                    request.destination = std::filesystem::path(audioDestination_.data());
                    request.bitrate = static_cast<std::uint32_t>(std::max(audioBitrateKbps_, 1)) * 1000U;
                    request.overwrite = audioOverwrite_;
                    if (audioRangeMode_ == 1) {
                        request.startSeconds = static_cast<double>(audioFromSeconds_);
                        request.endSeconds = static_cast<double>(audioToSeconds_);
                    }
                    try {
                        audioExportJob_.start(std::move(request));
                    } catch (const std::exception& exception) {
                        ImGui::TextWrapped("Export error: %s", exception.what());
                    }
                }
            }
        }
        if (!audioExportJob_.running()) {
            ImGui::Separator();
            if (ImGui::Button("Close"))
                ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
#endif
}

void Application::buildVideoExportUi() {
#if DAYO_HAS_IMGUI
    if (uiState_.videoExportOpen) {
        ImGui::OpenPopup("Export Video");
        uiState_.videoExportOpen = false;
    }
    if (ImGui::BeginPopupModal("Export Video", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!DAYO_HAS_MEDIA) {
            ImGui::TextUnformatted("FFmpeg support is not available in this build.");
        } else {
            ImGui::BeginDisabled(videoExportUiActive_);
            if (!videoRangeInitialized_) {
                videoFromFrame_ = 0;
                videoToFrame_ = scene_.timeline().duration > 0.0F
                                    ? static_cast<std::uint64_t>(std::ceil(scene_.timeline().duration))
                                    : 0U;
                videoRangeInitialized_ = true;
            }
            if (videoDestination_[0] == '\0' && !audioSource_.empty()) {
                const auto destination = audioSource_.parent_path() / (audioSource_.stem().string() + ".mp4");
                const auto text = destination.string();
                const auto count = std::min(text.size(), videoDestination_.size() - 1U);
                std::copy_n(text.data(), count, videoDestination_.data());
                videoDestination_[count] = '\0';
            }
            ImGui::TextUnformatted("Format");
            ImGui::SameLine(120.0F);
            ImGui::TextUnformatted("MP4 / H.264, H.265 or AV1");
            ImGui::TextUnformatted("Source");
            ImGui::SameLine(120.0F);
            if (audioSource_.empty())
                ImGui::TextUnformatted("No audio source (optional)");
            else
                ImGui::TextWrapped("%s", audioSource_.string().c_str());
            ImGui::InputText("Output", videoDestination_.data(), videoDestination_.size());
            int width = static_cast<int>(videoWidth_);
            int height = static_cast<int>(videoHeight_);
            if (ImGui::InputInt("Width", &width))
                videoWidth_ = static_cast<std::uint32_t>(std::max(width, 1));
            if (ImGui::InputInt("Height", &height))
                videoHeight_ = static_cast<std::uint32_t>(std::max(height, 1));
            ImGui::InputFloat("FPS", &videoFps_, 0.0F, 0.0F, "%.3f");
            videoFps_ = std::max(videoFps_, 1.0F);
            ImGui::Combo("Codec", &videoCodec_, "H.264\0H.265 / HEVC\0AV1\0");
            ImGui::SliderInt("Video bitrate (kbps)", &videoBitrateKbps_, 500, 50000);
            ImGui::SliderInt("Audio bitrate (kbps)", &videoAudioBitrateKbps_, 64, 512);
            ImGui::Checkbox("Include audio", &videoIncludeAudio_);
            auto from = static_cast<unsigned long long>(videoFromFrame_);
            auto to = static_cast<unsigned long long>(videoToFrame_);
            if (ImGui::InputScalar("From frame", ImGuiDataType_U64, &from))
                videoFromFrame_ = from;
            if (ImGui::InputScalar("To frame", ImGuiDataType_U64, &to))
                videoToFrame_ = to;
            ImGui::Checkbox("Overwrite existing file", &audioOverwrite_);
            ImGui::EndDisabled();
            ImGui::Separator();

            if (videoExportUiActive_) {
                if (videoExportFramesFinished_ && !videoExportJob_.running())
                    ImGui::TextUnformatted("Restoring preview...");
                else if (!videoPreRollDone_)
                    ImGui::Text("Preparing frame %llu / %llu",
                                static_cast<unsigned long long>(videoEvaluationNextFrame_),
                                static_cast<unsigned long long>(videoFromFrame_));
                ImGui::ProgressBar(videoExportJob_.progress(), {-1.0F, 0.0F});
                ImGui::Text("%.1f%%", static_cast<double>(videoExportJob_.progress()) * 100.0);
                if (ImGui::Button("Cancel video export")) {
                    videoExportJob_.requestCancel();
                    videoExportFramesFinished_ = true;
                    videoExportStatus_ = "Cancelled";
                }
            } else {
                if (!videoExportStatus_.empty())
                    ImGui::TextWrapped("%s", videoExportStatus_.c_str());
                if (const auto error = videoExportJob_.error()) {
                    ImGui::TextWrapped("Export error: %s", error->c_str());
                }
                if (const auto result = videoExportJob_.result()) {
                    ImGui::TextWrapped("Exported: %s (%.2f s)", result->output.string().c_str(),
                                       result->durationSeconds);
                }
                if (ImGui::Button("Export video")) {
                    try {
                        if (videoDestination_[0] == '\0')
                            throw std::invalid_argument("video output is empty");
                        if (videoToFrame_ < videoFromFrame_)
                            throw std::invalid_argument("video frame range is reversed");
                        core::VideoExportRequest request;
                        request.destination = videoDestination_.data();
                        request.width = videoWidth_;
                        request.height = videoHeight_;
                        request.fps = videoFps_;
                        request.codec = static_cast<core::VideoCodec>(videoCodec_);
                        request.bitrate = static_cast<std::uint32_t>(std::max(videoBitrateKbps_, 1)) * 1000U;
                        request.audioBitrate = static_cast<std::uint32_t>(std::max(videoAudioBitrateKbps_, 1)) * 1000U;
                        request.overwrite = audioOverwrite_;
                        request.includeAudio = videoIncludeAudio_ && !audioSource_.empty();
                        std::optional<std::filesystem::path> audioSource;
                        if (request.includeAudio)
                            audioSource = audioSource_;
                        videoExportRestoreFrame_ = animationFrame_;
                        videoExportRestoreMediaSeconds_ = mediaSeconds_;
                        videoExportRestorePlaying_ = playing_;
                        videoExportRestoreManualCamera_ = manualCamera_;
                        videoExportRestoreAudioActive_ = audioPlayer_.active();
                        videoExportRestorePending_ = true;
                        videoRestoreNextFrame_ = 0;
                        videoEvaluationNextFrame_ = 0;
                        videoSourceFps_ = sceneTimelineFps(scene_);
                        request.audioStartSeconds = static_cast<double>(videoFromFrame_) / videoSourceFps_;
                        videoOutputFrameCount_ =
                            videoOutputFrameCount(videoFromFrame_, videoToFrame_, videoSourceFps_, request.fps);
                        activeVideoExport_ = ActiveVideoExport{request.width, request.height};
                        videoExportJob_.start(std::move(request), std::move(audioSource), videoOutputFrameCount_);
                        videoNextFrame_ = 0;
                        videoPreviousSourceFrame_ = 0.0F;
                        videoPreRollDone_ = false;
                        videoExportFramesFinished_ = false;
                        videoExportUiActive_ = true;
                        playing_ = false;
                        videoExportStatus_.clear();
                        audioPlayer_.stop();
                    } catch (const std::exception& exception) {
                        if (videoExportRestorePending_) {
                            try {
                                restoreVideoExportState();
                                videoExportFramesFinished_ = true;
                                videoExportUiActive_ = videoExportRestorePending_;
                            } catch (const std::exception& restoreException) {
                                videoExportStatus_ = std::string("Export error: ") + exception.what() +
                                                     "; restore error: " + restoreException.what();
                                return;
                            }
                        }
                        videoExportStatus_ = std::string("Export error: ") + exception.what();
                    }
                }
            }
        }
        if (!videoExportUiActive_) {
            ImGui::Separator();
            if (ImGui::Button("Close"))
                ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
#endif
}

} // namespace dayo::app
