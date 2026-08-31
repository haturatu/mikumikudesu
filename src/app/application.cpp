#include "app/application.hpp"

#include "core/asset.hpp"
#include "core/animation.hpp"
#include "core/denoiser.hpp"
#include "core/image.hpp"
#include "core/log.hpp"
#include "core/model_probe.hpp"
#include "core/motion.hpp"
#include "core/vmdayo.hpp"
#include "core/video_export.hpp"
#include "graphics/device.hpp"
#include "platform/window.hpp"

#include <SDL3/SDL.h>

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
#include <fstream>
#include <iostream>
#include <limits>
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

core::VideoCodec parseVideoCodec(std::string_view value) {
    if (value == "h264") return core::VideoCodec::h264;
    if (value == "h265" || value == "hevc") return core::VideoCodec::h265;
    if (value == "av1") return core::VideoCodec::av1;
    throw std::invalid_argument("unknown video codec: " + std::string(value));
}

std::uint64_t parseFrameCount(std::string_view value) {
    std::uint64_t frames = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), frames);
    if (error != std::errc {} || end != value.data() + value.size() || frames == 0) {
        throw std::invalid_argument("--frames expects a positive integer");
    }
    return frames;
}

std::uint64_t parseNonNegativeFrame(std::string_view value, std::string_view option) {
    std::uint64_t frame = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), frame);
    if (error != std::errc {} || end != value.data() + value.size()) {
        throw std::invalid_argument(std::string(option) + " expects a non-negative integer");
    }
    return frame;
}

std::uint32_t parsePositiveInteger(std::string_view value, std::string_view option) {
    std::uint32_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc {} || end != value.data() + value.size() || result == 0) {
        throw std::invalid_argument(std::string(option) + " expects a positive integer");
    }
    return result;
}

double sceneTimelineFps(const core::Scene& scene) noexcept {
    const auto value = static_cast<double>(scene.timeline().fps);
    return std::isfinite(value) && value > 0.0 ? value : 30.0;
}

std::uint64_t videoOutputFrameCount(std::uint64_t firstFrame,
                                    std::uint64_t lastFrame,
                                    double sourceFps,
                                    double outputFps) {
    const auto intervals = static_cast<double>(lastFrame - firstFrame) * outputFps / sourceFps;
    if (!std::isfinite(intervals)
        || intervals > static_cast<double>(std::numeric_limits<std::uint64_t>::max() - 1U)) {
        throw std::invalid_argument("video frame range produces too many output frames");
    }
    return static_cast<std::uint64_t>(std::ceil(std::max(intervals, 0.0) - 1e-9)) + 1U;
}

float videoSourceFrame(std::uint64_t outputFrame,
                       std::uint64_t firstFrame,
                       std::uint64_t lastFrame,
                       double sourceFps,
                       double outputFps) noexcept {
    const auto value = static_cast<double>(firstFrame)
        + static_cast<double>(outputFrame) * sourceFps / outputFps;
    return static_cast<float>(std::min(static_cast<double>(lastFrame), value));
}

double parseSeconds(std::string_view value, std::string_view option) {
    try {
        std::size_t parsed = 0;
        const std::string text(value);
        const double result = std::stod(text, &parsed);
        if (parsed != text.size() || !std::isfinite(result) || result < 0.0) {
            throw std::invalid_argument("");
        }
        return result;
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string(option) + " expects a finite non-negative number");
    }
}

double parsePositiveNumber(std::string_view value, std::string_view option) {
    try {
        std::size_t parsed = 0;
        const std::string text(value);
        const double result = std::stod(text, &parsed);
        if (parsed != text.size() || !std::isfinite(result) || result <= 0.0) {
            throw std::invalid_argument("");
        }
        return result;
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string(option) + " expects a finite positive number");
    }
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
        } else if (argument == "--export-m4a") {
            if (++i >= argc) throw std::invalid_argument("--export-m4a requires a destination path");
            if (!options.audioExport) options.audioExport.emplace();
            options.audioExport->destination = argv[i];
        } else if (argument == "--export-video") {
            if (++i >= argc) throw std::invalid_argument("--export-video requires a destination path");
            if (!options.videoExport) options.videoExport.emplace();
            options.videoExport->destination = argv[i];
        } else if (argument == "--video-width") {
            if (++i >= argc) throw std::invalid_argument("--video-width requires a value");
            if (!options.videoExport) options.videoExport.emplace();
            options.videoExport->width = parsePositiveInteger(argv[i], "--video-width");
        } else if (argument == "--video-height") {
            if (++i >= argc) throw std::invalid_argument("--video-height requires a value");
            if (!options.videoExport) options.videoExport.emplace();
            options.videoExport->height = parsePositiveInteger(argv[i], "--video-height");
        } else if (argument == "--video-fps") {
            if (++i >= argc) throw std::invalid_argument("--video-fps requires a value");
            if (!options.videoExport) options.videoExport.emplace();
            options.videoExport->fps = parsePositiveNumber(argv[i], "--video-fps");
        } else if (argument == "--video-codec") {
            if (++i >= argc) throw std::invalid_argument("--video-codec requires a value");
            if (!options.videoExport) options.videoExport.emplace();
            options.videoExport->codec = parseVideoCodec(argv[i]);
        } else if (argument == "--video-bitrate") {
            if (++i >= argc) throw std::invalid_argument("--video-bitrate requires a value in kbps");
            const auto kbps = parsePositiveInteger(argv[i], "--video-bitrate");
            if (kbps > std::numeric_limits<std::uint32_t>::max() / 1000U) {
                throw std::invalid_argument("--video-bitrate is too large");
            }
            if (!options.videoExport) options.videoExport.emplace();
            options.videoExport->bitrate = kbps * 1000U;
        } else if (argument == "--video-from-frame") {
            if (++i >= argc) throw std::invalid_argument("--video-from-frame requires a value");
            if (!options.videoExport) options.videoExport.emplace();
            options.videoExport->fromFrame = parseNonNegativeFrame(argv[i], "--video-from-frame");
        } else if (argument == "--video-to-frame") {
            if (++i >= argc) throw std::invalid_argument("--video-to-frame requires a value");
            if (!options.videoExport) options.videoExport.emplace();
            options.videoExport->toFrame = parseNonNegativeFrame(argv[i], "--video-to-frame");
        } else if (argument == "--no-audio") {
            if (!options.videoExport) options.videoExport.emplace();
            options.videoExport->includeAudio = false;
        } else if (argument == "--audio-source") {
            if (++i >= argc) throw std::invalid_argument("--audio-source requires a path");
            if (options.videoExport) options.videoExport->audioSource = std::filesystem::path(argv[i]);
            else {
                if (!options.audioExport) options.audioExport.emplace();
                options.audioExport->source = std::filesystem::path(argv[i]);
            }
        } else if (argument == "--audio-bitrate") {
            if (++i >= argc) throw std::invalid_argument("--audio-bitrate requires a value in kbps");
            const auto kbps = parsePositiveInteger(argv[i], "--audio-bitrate");
            if (kbps > std::numeric_limits<std::uint32_t>::max() / 1000U) {
                throw std::invalid_argument("--audio-bitrate is too large");
            }
            if (options.videoExport) options.videoExport->audioBitrate = kbps * 1000U;
            else {
                if (!options.audioExport) options.audioExport.emplace();
                options.audioExport->bitrate = kbps * 1000U;
            }
        } else if (argument == "--audio-from") {
            if (++i >= argc) throw std::invalid_argument("--audio-from requires seconds");
            if (options.videoExport) {
                throw std::invalid_argument("--audio-from is only available with --export-m4a");
            }
            if (!options.audioExport) options.audioExport.emplace();
            options.audioExport->startSeconds = parseSeconds(argv[i], "--audio-from");
        } else if (argument == "--audio-to") {
            if (++i >= argc) throw std::invalid_argument("--audio-to requires seconds");
            if (options.videoExport) {
                throw std::invalid_argument("--audio-to is only available with --export-m4a");
            }
            if (!options.audioExport) options.audioExport.emplace();
            options.audioExport->endSeconds = parseSeconds(argv[i], "--audio-to");
        } else if (argument == "--overwrite") {
            if (options.videoExport) options.videoExport->overwrite = true;
            else {
                if (!options.audioExport) options.audioExport.emplace();
                options.audioExport->overwrite = true;
            }
        } else if (argument == "--help" || argument == "-h") {
            log::info("Usage: mikumikudesu [--probe] [--hidden] [--frames N] "
                      "[--renderer preview|subayai|bdpt] [--asset PATH] "
                      "[--save-project PATH] [--export-m4a PATH] [--audio-source PATH] "
                      "[--audio-bitrate KBPS] [--audio-from SEC] [--audio-to SEC] "
                      "[--export-video PATH] [--video-width PX] [--video-height PX] "
                      "[--video-fps FPS] [--video-codec h264|h265|av1] "
                      "[--video-bitrate KBPS] [--audio-bitrate KBPS] "
                      "[--video-from-frame N] [--video-to-frame N] [--no-audio] "
                      "[--overwrite] [--no-validation]");
            options.probeOnly = true;
        } else if (!argument.empty() && argument.front() == '-') {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        } else {
            options.assets.emplace_back(argument);
        }
    }
    if (options.videoExport && options.audioExport && options.audioExport->destination.empty()) {
        if (options.audioExport->source) options.videoExport->audioSource = options.audioExport->source;
        options.videoExport->audioBitrate = options.audioExport->bitrate;
        options.videoExport->overwrite = options.videoExport->overwrite || options.audioExport->overwrite;
        options.audioExport.reset();
    }
    return options;
}

Application::Application(Options options) : options_(std::move(options)) {}

void Application::resetProjectRuntimeState() {
    videoExportJob_.cancel();
    videoExportUiActive_ = false;
    videoExportFramesFinished_ = false;
    videoRangeInitialized_ = false;
    scene_.clearProjectState();
    if (device_ != nullptr) device_->clearPreviewResources();
    effectReloader_.reset();
    audioPlayer_.stop();
    audioSource_.clear();
    audioDestination_.fill('\0');
    audioFromSeconds_ = 0.0F;
    audioToSeconds_ = 0.0F;
    textures_.clear();
    animatedIndices_.clear();
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

    for (const auto& asset : options_.assets) handleAsset(asset);
    if (options_.saveProject) {
        core::saveProject(*options_.saveProject, currentProject());
        log::info("Saved project: ", options_.saveProject->string());
    }
    if (options_.probeOnly) {
        std::cout << device->capabilities().json() << '\n';
        return 0;
    }
    if (options_.videoExport) return runVideoExport();

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
        if (videoExportUiActive_) {
            if (videoExportJob_.running() && !videoExportFramesFinished_) {
                const auto& exportOptions = *options_.videoExport;
                if (videoNextFrame_ < videoOutputFrameCount_) {
                    if (!videoPreRollDone_) {
                        animationFrame_ = 0.0F;
                        scene_.setFrame(animationFrame_);
                        refreshAnimatedMesh(true, 0.0F);
                        refreshPreviewScene();
                        if (videoFromFrame_ > 0U) {
                            const auto sourceFrameDuration = static_cast<float>(1.0 / videoSourceFps_);
                            for (std::uint64_t frame = 1; frame <= videoFromFrame_; ++frame) {
                                animationFrame_ = static_cast<float>(frame);
                                scene_.setFrame(animationFrame_);
                                refreshAnimatedMesh(false, sourceFrameDuration);
                                refreshPreviewScene();
                            }
                        }
                        videoPreRollDone_ = true;
                        videoPreviousSourceFrame_ = static_cast<float>(videoFromFrame_);
                    }
                    const auto sourceFrame = videoSourceFrame(
                        videoNextFrame_, videoFromFrame_, videoToFrame_, videoSourceFps_, videoFps_);
                    if (videoNextFrame_ != 0U) {
                        animationFrame_ = sourceFrame;
                        scene_.setFrame(animationFrame_);
                        const auto delta = std::max(0.0F, sourceFrame - videoPreviousSourceFrame_)
                            / static_cast<float>(videoSourceFps_);
                        refreshAnimatedMesh(false, delta);
                    }
                    if (videoMode_) {
                        mediaSeconds_ = std::max(0.0, static_cast<double>(sourceFrame) / videoSourceFps_);
                        refreshVideoFrame();
                    }
                    refreshPreviewScene();
                    try {
                        videoExportJob_.submitFrame(device_->renderToImage(
                            { exportOptions.width, exportOptions.height }));
                    } catch (const std::exception& exception) {
                        videoExportStatus_ = exception.what();
                        videoExportJob_.cancel();
                        videoExportFramesFinished_ = true;
                        videoExportUiActive_ = false;
                    }
                    videoPreviousSourceFrame_ = sourceFrame;
                    ++videoNextFrame_;
                } else {
                    videoExportJob_.finishFrames();
                    videoExportFramesFinished_ = true;
                }
            } else if (!videoExportFramesFinished_ && !videoExportJob_.running()) {
                const auto error = videoExportJob_.error();
                videoExportStatus_ = error ? *error : "Video export stopped unexpectedly";
                videoExportFramesFinished_ = true;
                videoExportUiActive_ = false;
            }
        } else if (scene_.advanceFrame(deltaSeconds * playbackSpeed_, playing_)) {
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
            mediaSeconds_ += deltaSeconds * static_cast<double>(playbackSpeed_);
            if (media->info().durationSeconds > 0.0 && mediaSeconds_ >= media->info().durationSeconds) {
                if (repeat_) mediaSeconds_ = std::fmod(mediaSeconds_, media->info().durationSeconds);
                else {
                    mediaSeconds_ = media->info().durationSeconds;
                    playing_ = false;
                    audioPlayer_.setPaused(true);
                }
                uploadedVideoFrame_ = -1;
                if (repeat_ && media->info().hasAudio) {
                    if (loadedAudio_.samples.empty()) loadedAudio_ = media->decodeAudio();
                    audioPlayer_.play(loadedAudio_, std::max(0.0F, audioOffsetSeconds_));
                    audioPlayer_.setVolume(audioVolume_);
                }
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

core::DayoProject Application::currentProject() const {
    core::DayoProject project;
    project.renderer = device_ == nullptr ? "preview"
        : std::string(graphics::toString(device_->activeRenderer()));
    project.frame = animationFrame_;
    project.playing = playing_;
    project.assets = projectAssets_;
    core::VmdMotion camera = scene_.cameraMotion() == nullptr
        ? core::VmdMotion {} : *scene_.cameraMotion();
    if (camera.modelName.empty()) camera.modelName = "Camera/Light";
    project.embeddedMotions.push_back(std::move(camera));
    for (const auto& model : scene_.models()) {
        auto motion = model.motion == nullptr ? core::VmdMotion {} : *model.motion;
        if (motion.modelName.empty()) motion.modelName = model.displayName;
        project.embeddedMotions.push_back(std::move(motion));
    }
    return project;
}

int Application::runVideoExport() {
    if (!options_.videoExport) throw std::invalid_argument("--export-video is required");
    const auto& options = *options_.videoExport;
    if (options.destination.empty()) throw std::invalid_argument("--export-video requires a destination path");
    if (!core::canExportVideo(options.codec)) {
        throw std::runtime_error("requested video encoder is unavailable");
    }
    if (device_ == nullptr) throw std::logic_error("video export has no graphics device");

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
                if (kind != core::AssetKind::audio && kind != core::AssetKind::video) continue;
                const auto source = std::filesystem::absolute(asset);
                core::MediaFile media(source);
                if (media.info().hasAudio
                    && std::find(candidates.begin(), candidates.end(), source) == candidates.end()) {
                    candidates.push_back(source);
                }
            }
            if (candidates.size() > 1) {
                throw std::runtime_error("multiple audio sources found; use --audio-source PATH");
            }
            if (candidates.size() == 1) audioSource = candidates.front();
            else if (!audioSource_.empty()) audioSource = audioSource_;
        }
        if (!audioSource) {
            log::warn("Video export: no audio source found; exporting video only");
        }
    }

    const auto timelineEnd = scene_.timeline().duration > 0.0F
        ? static_cast<std::uint64_t>(std::ceil(scene_.timeline().duration)) : 0U;
    const auto firstFrame = options.fromFrame.value_or(0U);
    const auto lastFrame = options.toFrame.value_or(timelineEnd);
    if (lastFrame < firstFrame) throw std::invalid_argument("video frame range is reversed");
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
    request.includeAudio = audioSource.has_value();
    request.audioBitrate = options.audioBitrate;
    request.overwrite = options.overwrite;

    core::VideoExporter exporter(request);
    if (audioSource) {
        core::MediaFile media(*audioSource);
        const auto maxSamples = static_cast<std::uint64_t>(std::ceil(
            static_cast<double>(frameCount) / options.fps * 48'000.0)) * 2U;
        std::uint64_t writtenSamples = 0;
        media.streamAudio([&](std::span<const float> samples,
                              std::uint32_t sampleRate,
                              std::uint32_t channels) {
            if (writtenSamples >= maxSamples) return;
            const auto count = std::min<std::uint64_t>(samples.size(), maxSamples - writtenSamples);
            exporter.writeAudio(samples.first(static_cast<std::size_t>(count)), sampleRate, channels);
            writtenSamples += count;
        });
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
        if (videoMode_) refreshVideoFrame();
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
            const auto deltaSeconds = std::max(0.0F, sourceFrame - previousSourceFrame)
                * sourceFrameDuration;
            evaluateFrame(sourceFrame, deltaSeconds, false);
        }
        previousSourceFrame = sourceFrame;
        const auto image = device_->renderToImage({ request.width, request.height });
        exporter.writeVideoFrame(image);
        if (outputFrame + 1U == frameCount || outputFrame == 0U
            || (outputFrame + 1U) % std::max<std::uint64_t>(1U, frameCount / 20U) == 0U) {
            log::info("Video export: ", outputFrame + 1U, "/", frameCount, " frames");
        }
    }
    device_->waitIdle();
    const auto result = exporter.finish();
    log::info("Exported MP4: ", result.output.string(), " (",
              result.durationSeconds, " s, ", result.encodedFrames, " frames)");
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
            resetProjectRuntimeState();
            if (project.renderer == "subayai") device_->selectRenderer(graphics::RendererKind::subayai);
            else if (project.renderer == "bdpt") device_->selectRenderer(graphics::RendererKind::bdpt);
            else device_->selectRenderer(graphics::RendererKind::preview);
            for (const auto& asset : project.assets) handleAsset(asset.path);
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
            scene_.attachMotion(document.motion, scene_.selectedModelId(), document.modelName);
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
                {{ -1.0F, -1.0F, 0.0F }, {}, { 0.0F, 1.0F }},
                {{  1.0F, -1.0F, 0.0F }, {}, { 1.0F, 1.0F }},
                {{  1.0F,  1.0F, 0.0F }, {}, { 1.0F, 0.0F }},
                {{ -1.0F,  1.0F, 0.0F }, {}, { 0.0F, 0.0F }},
            }};
            const std::array<std::uint32_t, 6> indices { 0, 1, 2, 2, 3, 0 };
            if (scene_.models().empty()) {
                device_->uploadPreviewMesh(vertices, indices);
                refreshPreviewBackground();
                device_->updatePreviewMaterials(std::span<const graphics::PreviewMaterial> {});
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
            if (videoMode_) refreshVideoFrame();
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
                                peak = std::max(peak, std::abs(loadedAudio_.samples[frame * loadedAudio_.channels + channel]));
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
                refreshVideoFrame();
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
            auto motion = core::loadVmd(path);
            const bool cameraOnly = motion.bones.empty() && motion.morphs.empty()
                && (!motion.cameras.empty() || !motion.lights.empty());
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
            if (selectedModel() != nullptr) refreshAnimatedMesh(false);
            refreshPreviewScene();
            if (cameraOnly) {
                lastAsset_ = "VMD camera/light — " + std::to_string(cameraKeyCount) + " camera keys, "
                           + std::to_string(lightKeyCount) + " light keys, "
                           + std::to_string(lastFrame) + " frames";
            } else {
                lastAsset_ = "VMD " + (motionName.empty() ? path.filename().string() : motionName)
                           + " — " + std::to_string(boneKeyCount) + " bone keys, "
                           + std::to_string(morphKeyCount) + " morph keys, "
                           + std::to_string(lastFrame) + " frames";
            }
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
    std::size_t vertexCount = 0;
    std::size_t indexCount = 0;
    std::size_t materialCount = 0;
    for (const auto& instance : scene_.models()) {
        if (!instance.visible || instance.model == nullptr || instance.animator == nullptr) continue;
        const auto clones = std::max(instance.cloneCount, 1U);
        vertexCount += instance.model->vertices.size() * clones;
        indexCount += instance.model->indices.size() * clones;
        materialCount += instance.model->materials.size() * clones;
    }
    const bool rebuildTopology = initialUpload
        || animatedTopologyGeneration_ != scene_.topologyGeneration()
        || animatedIndices_.size() != indexCount
        || animatedMaterialTemplates_.size() != materialCount;
    std::vector<graphics::PreviewVertex> vertices;
    vertices.reserve(vertexCount);
    std::vector<graphics::PreviewMaterial> materials = rebuildTopology
        ? std::vector<graphics::PreviewMaterial> {} : animatedMaterialTemplates_;
    if (rebuildTopology) {
        animatedIndices_.clear();
        animatedIndices_.reserve(indexCount);
        materials.reserve(materialCount);
    }
    std::size_t materialCursor = 0;
    std::uint32_t indexCursor = 0;
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
        auto frame = instance.animator->evaluate(animationFrame_, deltaSeconds);
        if (!frame.vertices.empty() && (initialUpload || static_cast<int>(animationFrame_) % 30 == 0)) {
            const auto& vertex = frame.vertices.front().position;
            log::debug("Animation sample: model=", instance.displayName,
                       ", frame=", animationFrame_,
                       ", vertex0=(", vertex[0], ",", vertex[1], ",", vertex[2], ")");
        }
        if (instance.softBody != nullptr && instance.softBody->available()) {
            instance.softBody->step(deltaSeconds, { gravity.gravityDirection[0] * gravity.gravity,
                                                    gravity.gravityDirection[1] * gravity.gravity,
                                                    gravity.gravityDirection[2] * gravity.gravity });
            instance.softBody->apply(frame.vertices);
        }
        core::normalizeForPreview(frame.vertices, instance.normalization);
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
            const auto firstCloneIndex = indexCursor;
            const float cloneOffset = (static_cast<float>(clone)
                                     - static_cast<float>(cloneCount - 1U) * 0.5F) * 2.2F;
            for (const auto& source : frame.vertices) {
                graphics::PreviewVertex vertex;
                std::memcpy(vertex.position, source.position.data(), sizeof(vertex.position));
                vertex.position[0] += cloneOffset;
                std::memcpy(vertex.normal, source.normal.data(), sizeof(vertex.normal));
                std::memcpy(vertex.uv, source.uv.data(), sizeof(vertex.uv));
                vertices.push_back(vertex);
            }
            if (rebuildTopology) {
                for (const auto index : instance.model->indices) animatedIndices_.push_back(baseVertex + index);
            }
            indexCursor += static_cast<std::uint32_t>(instance.model->indices.size());
            std::uint32_t firstIndex = firstCloneIndex;
            for (std::size_t materialIndex = 0; materialIndex < instance.model->materials.size(); ++materialIndex) {
                if (rebuildTopology) {
                    graphics::PreviewMaterial material;
                    material.firstIndex = firstIndex;
                    material.indexCount = instance.model->materials[materialIndex].indexCount;
                    material.doubleSided = (instance.model->materials[materialIndex].drawFlags & 0x01U) != 0;
                    material.textureSlot = instance.model->materials[materialIndex].textureIndex >= 0
                        ? textureBase + static_cast<std::uint32_t>(instance.model->materials[materialIndex].textureIndex) + 1U : 0U;
                    materials.push_back(material);
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
                }
                firstIndex += material.indexCount;
            }
        }
    }
    if (vertices.empty() || animatedIndices_.empty()) return;
    if (rebuildTopology) {
        animatedMaterialTemplates_ = materials;
        animatedTopologyGeneration_ = scene_.topologyGeneration();
    }
    if (initialUpload || rebuildTopology) device_->uploadPreviewMesh(vertices, animatedIndices_);
    else {
        try { device_->updatePreviewVertices(vertices); }
        catch (const std::exception&) { device_->uploadPreviewMesh(vertices, animatedIndices_); }
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

void Application::refreshPreviewBackground() {
    if (device_ == nullptr) return;
    const auto& background = scene_.background();
    if (background.screenSource != core::ScreenTextureSource::backgroundImage
        || !background.image.has_value()) {
        device_->uploadPreviewBackground({});
        return;
    }
    const auto& image = *background.image;
    const std::array textures { graphics::PreviewTexture { image.width, image.height, image.pixels } };
    device_->uploadPreviewBackground(textures);
}

void Application::refreshVideoFrame() {
    auto* media = scene_.media();
    if (!videoMode_ || media == nullptr || device_ == nullptr) return;
    const auto frameIndex = static_cast<std::int64_t>(mediaSeconds_ * media->info().videoFramesPerSecond);
    if (frameIndex == uploadedVideoFrame_) return;
    const auto image = media->decodeVideoFrame(mediaSeconds_);
    scene_.setBackgroundScreenSource(core::ScreenTextureSource::backgroundVideo);
    const std::array textures { graphics::PreviewTexture { image.width, image.height, image.pixels } };
    device_->uploadPreviewBackground(textures);
    if (scene_.models().empty()) device_->updatePreviewMaterials({});
    uploadedVideoFrame_ = frameIndex;
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
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            if (recordCamera_) {
                core::VmdMotion before = scene_.cameraMotion() ? *scene_.cameraMotion() : core::VmdMotion {};
                auto document = core::toMotionDocument(before);
                core::VmdCameraKey key = editedCamera_;
                key.frame = static_cast<std::uint32_t>(std::max(animationFrame_, 0.0F));
                key.distance = -cameraDistance_ / std::max(normalization_.scale, 0.0001F);
                key.rotation = { cameraPitch_, cameraYaw_, 0.0F };
                std::erase_if(document.cameras, [&](const auto& item) { return item.frame == key.frame; });
                document.cameras.push_back(key);
                core::MotionEditor::normalize(document);
                history_.execute(scene_, std::make_unique<core::EditMotionCommand>(0, true, before,
                    core::toVmdMotion(std::move(document), before.modelName), "Record camera key"));
            } else {
                playing_ = !playing_;
                if (audioPlayer_.active()) audioPlayer_.setPaused(!playing_);
            }
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            if (history_.undo(scene_)) {
                animationFrame_ = scene_.timeline().frame;
                refreshAnimatedMesh(false);
                refreshPreviewScene();
            }
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
            if (history_.redo(scene_)) {
                animationFrame_ = scene_.timeline().frame;
                refreshAnimatedMesh(false);
                refreshPreviewScene();
            }
        }
    }
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
        ImGui::Checkbox("Repeat", &repeat_);
        ImGui::SliderFloat("Playback speed", &playbackSpeed_, 0.1F, 4.0F, "%.2fx");
        if (ImGui::SliderFloat("Volume", &audioVolume_, 0.0F, 1.0F)) audioPlayer_.setVolume(audioVolume_);
        if (ImGui::DragFloat("Audio offset", &audioOffsetSeconds_, 0.01F, -60.0F, 60.0F, "%.2f s")
            && !loadedAudio_.samples.empty()) {
            audioPlayer_.play(loadedAudio_, std::max(0.0F, audioOffsetSeconds_));
            audioPlayer_.setVolume(audioVolume_);
            audioPlayer_.setPaused(!playing_);
        }
        if (!waveformPeaks_.empty()) {
            ImGui::PlotLines("Waveform", waveformPeaks_.data(), static_cast<int>(waveformPeaks_.size()),
                0, nullptr, 0.0F, 1.0F, { 0.0F, 72.0F });
        }
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
        ImGui::SameLine();
        if (ImGui::Button("Update one frame") && model != nullptr) {
            refreshAnimatedMesh(false, 1.0F / 30.0F);
            refreshPreviewScene();
        }
        ImGui::Checkbox("Rigid body debug", &physicsDebug_);
        if (physicsDebug_ && model != nullptr && model->physics != nullptr) {
            const auto count = model->physics->bodyCount();
            if (ImGui::BeginChild("rigid-body-debug", { 0.0F, 120.0F }, true)) {
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(count));
                while (clipper.Step()) for (int body = clipper.DisplayStart; body < clipper.DisplayEnd; ++body) {
                    const auto transform = model->physics->bodyTransform(static_cast<std::size_t>(body));
                    ImGui::Text("%d  mode %u  P %.2f %.2f %.2f", body,
                        model->physics->bodyMode(static_cast<std::size_t>(body)),
                        transform.position[0], transform.position[1], transform.position[2]);
                }
            }
            ImGui::EndChild();
        }
    }
    ImGui::End();
    buildEditorUi();
    buildAudioExportUi();
    buildVideoExportUi();
#endif
}

void Application::buildEditorUi() {
#if DAYO_HAS_IMGUI
    auto* model = selectedModel();
    const bool global = editGlobalMotion_;
    const auto* active = global ? scene_.cameraMotion() : (model != nullptr ? model->motion.get() : nullptr);
    const auto target = model != nullptr ? model->id : core::ModelId {};
    const auto execute = [&](core::VmdMotion before, core::MotionDocument document, bool globalMotion,
                             std::string label) {
        auto after = core::toVmdMotion(std::move(document), before.modelName);
        history_.execute(scene_, std::make_unique<core::EditMotionCommand>(
            target, globalMotion, std::move(before), std::move(after), std::move(label)));
        refreshAnimatedMesh(false);
        refreshPreviewScene();
    };

    if (ImGui::Begin("Keyframes")) {
        if (ImGui::Checkbox("Edit global camera/light motion", &editGlobalMotion_)) selectedKeys_.clear();
        if (active == nullptr) {
            ImGui::TextUnformatted("Load a VMD/VMdayo motion to edit keyframes.");
        } else {
            ImGui::Text("Bone %zu  Morph %zu  Camera %zu  Light %zu  Shadow %zu  IK %zu",
                active->bones.size(), active->morphs.size(), active->cameras.size(), active->lights.size(),
                active->shadows.size(), active->ik.size());
            auto row = [&](core::MotionTrack track, std::size_t index, std::uint32_t frame,
                           const std::string& name) {
                const core::MotionKeyRef key { track, index };
                const bool selected = std::find(selectedKeys_.begin(), selectedKeys_.end(), key) != selectedKeys_.end();
                const auto label = name + "  @ " + std::to_string(frame) + "##"
                    + std::to_string(static_cast<int>(track)) + ":" + std::to_string(index);
                if (!ImGui::Selectable(label.c_str(), selected)) return;
                if (!ImGui::GetIO().KeyCtrl) selectedKeys_.clear();
                const auto found = std::find(selectedKeys_.begin(), selectedKeys_.end(), key);
                if (found == selectedKeys_.end()) selectedKeys_.push_back(key); else selectedKeys_.erase(found);
            };
            if (ImGui::BeginChild("key-list", { 0.0F, 220.0F }, true)) {
                for (std::size_t i = 0; i < active->bones.size(); ++i) row(core::MotionTrack::bone, i, active->bones[i].frame, active->bones[i].name);
                for (std::size_t i = 0; i < active->morphs.size(); ++i) row(core::MotionTrack::morph, i, active->morphs[i].frame, active->morphs[i].name);
                for (std::size_t i = 0; i < active->cameras.size(); ++i) row(core::MotionTrack::camera, i, active->cameras[i].frame, "Camera");
                for (std::size_t i = 0; i < active->lights.size(); ++i) row(core::MotionTrack::light, i, active->lights[i].frame, "Light");
                for (std::size_t i = 0; i < active->shadows.size(); ++i) row(core::MotionTrack::shadow, i, active->shadows[i].frame, "Self shadow");
                for (std::size_t i = 0; i < active->ik.size(); ++i) row(core::MotionTrack::ik, i, active->ik[i].frame, "IK / visibility");
            }
            ImGui::EndChild();
            const bool keyWindowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
                && !ImGui::GetIO().WantTextInput;
            const bool copyShortcut = keyWindowFocused && ImGui::GetIO().KeyCtrl
                && ImGui::IsKeyPressed(ImGuiKey_C, false);
            const bool cutShortcut = keyWindowFocused && ImGui::GetIO().KeyCtrl
                && ImGui::IsKeyPressed(ImGuiKey_X, false);
            const bool pasteShortcut = keyWindowFocused && ImGui::GetIO().KeyCtrl
                && ImGui::IsKeyPressed(ImGuiKey_V, false);
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
                static_cast<void>(core::MotionEditor::paste(document, motionClipboard_,
                    static_cast<std::uint32_t>(std::max(animationFrame_, 0.0F))));
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
            static std::array<char, 1024> exportPath {};
            ImGui::InputText("VMD destination", exportPath.data(), exportPath.size());
            if (ImGui::Button("Export VMD") && exportPath[0] != '\0') {
                try { core::saveVmd(exportPath.data(), *active); lastAsset_ = "Exported VMD"; }
                catch (const std::exception& error) { lastAsset_ = error.what(); }
            }
        }
    }
    ImGui::End();

    if (ImGui::Begin("Bone / Expression")) {
        if (model == nullptr || model->model == nullptr) {
            ImGui::TextUnformatted("Select a model to edit bones and morphs.");
        } else {
            const auto& bones = model->model->bones;
            if (!bones.empty()) {
                selectedBone_ = std::clamp(selectedBone_, 0, static_cast<int>(bones.size() - 1));
                if (ImGui::BeginCombo("Bone", bones[static_cast<std::size_t>(selectedBone_)].name.c_str())) {
                    for (std::size_t i = 0; i < bones.size(); ++i) if (ImGui::Selectable(bones[i].name.c_str(), selectedBone_ == static_cast<int>(i))) selectedBone_ = static_cast<int>(i);
                    ImGui::EndCombo();
                }
                ImGui::DragFloat3("Translation", editedBoneTranslation_.data(), 0.01F);
                ImGui::DragFloat4("Rotation quaternion", editedBoneRotation_.data(), 0.01F, -1.0F, 1.0F);
                ImGui::Checkbox("Bone physics", &editedBonePhysics_);
                if (ImGui::Button("Register bone")) {
                    core::VmdMotion before = model->motion ? *model->motion : core::VmdMotion {};
                    before.modelName = model->displayName;
                    auto document = core::toMotionDocument(before);
                    const auto frame = static_cast<std::uint32_t>(std::max(animationFrame_, 0.0F));
                    const auto& name = bones[static_cast<std::size_t>(selectedBone_)].name;
                    std::erase_if(document.bones, [&](const auto& key) { return key.frame == frame && key.name == name; });
                    document.bones.push_back({ name, frame, editedBoneTranslation_, editedBoneRotation_, {}, editedBonePhysics_ });
                    core::MotionEditor::normalize(document);
                    execute(std::move(before), std::move(document), false, "Register bone key");
                }
            }
            const auto& morphs = model->model->morphs;
            if (!morphs.empty()) {
                selectedMorph_ = std::clamp(selectedMorph_, 0, static_cast<int>(morphs.size() - 1));
                if (ImGui::BeginCombo("Morph", morphs[static_cast<std::size_t>(selectedMorph_)].name.c_str())) {
                    for (std::size_t i = 0; i < morphs.size(); ++i) if (ImGui::Selectable(morphs[i].name.c_str(), selectedMorph_ == static_cast<int>(i))) selectedMorph_ = static_cast<int>(i);
                    ImGui::EndCombo();
                }
                ImGui::SliderFloat("Weight", &editedMorphWeight_, 0.0F, 1.0F);
                if (ImGui::Button("Register morph")) {
                    core::VmdMotion before = model->motion ? *model->motion : core::VmdMotion {};
                    before.modelName = model->displayName;
                    auto document = core::toMotionDocument(before);
                    const auto frame = static_cast<std::uint32_t>(std::max(animationFrame_, 0.0F));
                    const auto& name = morphs[static_cast<std::size_t>(selectedMorph_)].name;
                    std::erase_if(document.morphs, [&](const auto& key) { return key.frame == frame && key.name == name; });
                    document.morphs.push_back({ name, frame, editedMorphWeight_ });
                    core::MotionEditor::normalize(document);
                    execute(std::move(before), std::move(document), false, "Register morph key");
                }
            }
        }
    }
    ImGui::End();

    if (ImGui::Begin("Camera / Light / Self Shadow")) {
        ImGui::Checkbox("Realtime camera recording (Space registers)", &recordCamera_);
        ImGui::DragFloat3("Camera target", editedCamera_.position.data(), 0.01F);
        ImGui::DragFloat3("Camera rotation", editedCamera_.rotation.data(), 0.01F);
        ImGui::DragFloat("Camera distance", &editedCamera_.distance, 0.1F);
        int viewAngle = static_cast<int>(editedCamera_.viewAngle == 0 ? 30 : editedCamera_.viewAngle);
        if (ImGui::SliderInt("FoV", &viewAngle, 1, 179)) editedCamera_.viewAngle = static_cast<float>(viewAngle);
        ImGui::Checkbox("Perspective", &editedCamera_.perspective);
        ImGui::InputInt("Camera parent model", &editedCamera_.parentModel);
        ImGui::InputInt("Camera parent bone", &editedCamera_.parentBone);
        ImGui::InputText("Camera parent bone name", cameraParentBoneName_.data(), cameraParentBoneName_.size());
        if (ImGui::Button("Register camera")) {
            core::VmdMotion before = scene_.cameraMotion() ? *scene_.cameraMotion() : core::VmdMotion {};
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
            core::VmdMotion before = scene_.cameraMotion() ? *scene_.cameraMotion() : core::VmdMotion {};
            auto document = core::toMotionDocument(before);
            editedLight_.frame = static_cast<std::uint32_t>(std::max(animationFrame_, 0.0F));
            std::erase_if(document.lights, [&](const auto& key) { return key.frame == editedLight_.frame; });
            document.lights.push_back(editedLight_);
            core::MotionEditor::normalize(document);
            execute(std::move(before), std::move(document), true, "Register light key");
        }
        int shadowMode = editedShadow_.mode;
        if (ImGui::Combo("Self shadow", &shadowMode, "None\0Mode 1\0Mode 2\0")) editedShadow_.mode = static_cast<std::uint8_t>(shadowMode);
        ImGui::DragFloat("Shadow distance", &editedShadow_.distance, 0.1F, 0.0F, 10'000.0F);
        if (ImGui::Button("Register self shadow")) {
            core::VmdMotion before = scene_.cameraMotion() ? *scene_.cameraMotion() : core::VmdMotion {};
            auto document = core::toMotionDocument(before);
            editedShadow_.frame = static_cast<std::uint32_t>(std::max(animationFrame_, 0.0F));
            std::erase_if(document.shadows, [&](const auto& key) { return key.frame == editedShadow_.frame; });
            document.shadows.push_back(editedShadow_);
            core::MotionEditor::normalize(document);
            execute(std::move(before), std::move(document), true, "Register self shadow key");
        }
    }
    ImGui::End();

    if (ImGui::Begin("Project tools")) {
        ImGui::InputText("Project file", projectDestination_.data(), projectDestination_.size());
        if (ImGui::Button("Save Dayo 1.30 project")) {
            try {
                core::saveProject(projectDestination_.data(), currentProject());
                projectSaveStatus_ = "Project saved";
            } catch (const std::exception& error) {
                projectSaveStatus_ = error.what();
            }
        }
        if (!projectSaveStatus_.empty()) ImGui::TextWrapped("%s", projectSaveStatus_.c_str());
        ImGui::Separator();
        auto background = scene_.background();
        int source = static_cast<int>(background.screenSource);
        if (ImGui::Combo("Background source", &source, "Previous frame\0Video\0Image\0White\0")) scene_.setBackgroundScreenSource(static_cast<core::ScreenTextureSource>(source));
        bool enabled = background.enabled;
        if (ImGui::Checkbox("Background enabled", &enabled)) scene_.setBackgroundEnabled(enabled);
        bool crop = background.crop == core::ScreenCropMode::crop4x3;
        if (ImGui::Checkbox("Crop 4:3", &crop)) scene_.setBackgroundCrop(crop ? core::ScreenCropMode::crop4x3 : core::ScreenCropMode::none);
        bool alpha = background.mode == core::BackgroundMode::alpha;
        if (ImGui::Checkbox("Alpha background", &alpha)) scene_.setBackgroundMode(alpha ? core::BackgroundMode::alpha : core::BackgroundMode::opaque);
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
                    if (!SDL_OpenURL(url.c_str())) lastAsset_ = std::string("Open folder: ") + SDL_GetError();
                }
                ImGui::TreePop();
            }
            if (!model->materialSettings.empty() && ImGui::TreeNode("Material annotations")) {
                static int materialIndex = 0;
                materialIndex = std::clamp(materialIndex, 0, static_cast<int>(model->materialSettings.size() - 1));
                const auto& materials = model->model->materials;
                const char* preview = materials[static_cast<std::size_t>(materialIndex)].name.c_str();
                if (ImGui::BeginCombo("Material", preview)) {
                    for (std::size_t i = 0; i < materials.size(); ++i) if (ImGui::Selectable(materials[i].name.c_str(), materialIndex == static_cast<int>(i))) materialIndex = static_cast<int>(i);
                    ImGui::EndCombo();
                }
                static std::array<char, 1024> annotation {};
                ImGui::InputText("Annotation / MatDesc", annotation.data(), annotation.size());
                if (ImGui::Button("Apply material annotation")) {
                    model->materialSettings[static_cast<std::size_t>(materialIndex)].annotation = annotation.data();
                    scene_.markDirty(core::DirtyFlag::material | core::DirtyFlag::effect);
                }
                ImGui::TreePop();
            }
            if (scene_.models().size() > 1 && ImGui::TreeNode("External parent")) {
                static int parentIndex = 0;
                static std::array<char, 256> parentBone {};
                static std::array<char, 256> childBone {};
                parentIndex = std::clamp(parentIndex, 0, static_cast<int>(scene_.models().size() - 1));
                if (ImGui::BeginCombo("Parent model", scene_.models()[static_cast<std::size_t>(parentIndex)].displayName.c_str())) {
                    for (std::size_t i = 0; i < scene_.models().size(); ++i) {
                        if (ImGui::Selectable(scene_.models()[i].displayName.c_str(), parentIndex == static_cast<int>(i))) parentIndex = static_cast<int>(i);
                    }
                    ImGui::EndCombo();
                }
                ImGui::InputText("Parent bone", parentBone.data(), parentBone.size());
                ImGui::InputText("Child bone", childBone.data(), childBone.size());
                if (ImGui::Button("Add external parent")) {
                    std::string error;
                    const auto& parent = scene_.models()[static_cast<std::size_t>(parentIndex)];
                    if (!scene_.addExternalParent({ parent.id, parentBone.data(), model->id, childBone.data() }, &error)) {
                        lastAsset_ = "External parent: " + error;
                    } else {
                        core::VmdMotion before = model->motion ? *model->motion : core::VmdMotion {};
                        before.modelName = model->displayName;
                        auto document = core::toMotionDocument(before);
                        const auto frame = static_cast<std::uint32_t>(std::max(animationFrame_, 0.0F));
                        document.externalParents.push_back({ frame, static_cast<std::int32_t>(parent.id),
                                                             parentBone.data(), childBone.data() });
                        core::MotionEditor::normalize(document);
                        execute(std::move(before), std::move(document), false,
                                "Register external parent key");
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
                if (std::find(scanned.begin(), scanned.end(), directory) != scanned.end()) continue;
                scanned.push_back(directory);
                std::error_code directoryError;
                for (std::filesystem::directory_iterator iterator(directory, directoryError), end;
                     !directoryError && iterator != end; iterator.increment(directoryError)) {
                    auto extension = iterator->path().extension().string();
                    std::ranges::transform(extension, extension.begin(), [](unsigned char value) {
                        return static_cast<char>(std::tolower(value));
                    });
                    if (extension != ".url") continue;
                    std::ifstream input(iterator->path());
                    std::string line;
                    while (std::getline(input, line)) if (line.starts_with("URL=")) {
                        credits += "url: " + line.substr(4) + "\n";
                        break;
                    }
                }
            }
            if (model != nullptr && !model->model->metadata.comment.empty()) credits += model->model->metadata.comment + "\n";
            ImGui::SetClipboardText(credits.c_str());
        }
        if (ImGui::TreeNode("Shortcuts")) {
            ImGui::TextUnformatted("Space: Play / Pause\nCtrl+Z: Undo\nCtrl+Y: Redo\nCtrl+C/X/V: Copy/Cut/Paste keys\nDelete: Delete selected keys");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Image sequence output")) {
            static std::array<char, 1024> outputDirectory { 'o', 'u', 't', 'p', 'u', 't', '\0' };
            ImGui::InputText("Directory", outputDirectory.data(), outputDirectory.size());
            int first = static_cast<int>(sequenceOutput_.firstFrame);
            int last = static_cast<int>(sequenceOutput_.lastFrame);
            int samples = static_cast<int>(sequenceOutput_.samples);
            if (ImGui::InputInt("First frame", &first)) sequenceOutput_.firstFrame = static_cast<std::uint32_t>(std::max(first, 0));
            if (ImGui::InputInt("Last frame", &last)) sequenceOutput_.lastFrame = static_cast<std::uint32_t>(std::max(last, 0));
            if (ImGui::InputInt("Samples", &samples)) sequenceOutput_.samples = static_cast<std::uint32_t>(std::clamp(samples, 1, 4096));
            ImGui::Checkbox("Motion blur", &sequenceOutput_.motionBlur);
            int format = static_cast<int>(sequenceOutput_.format);
            if (ImGui::Combo("Format", &format, "PPM\0PNG\0EXR\0")) sequenceOutput_.format = static_cast<core::OutputFormat>(format);
            if (ImGui::Button("Render sequence")) {
                const auto restoreFrame = scene_.timeline().frame;
                try {
                    if (sequenceOutput_.lastFrame < sequenceOutput_.firstFrame) throw std::invalid_argument("last frame precedes first frame");
                    sequenceOutput_.directory = outputDirectory.data();
                    core::OutputQueue output(sequenceOutput_);
                    for (std::uint32_t frame = sequenceOutput_.firstFrame; frame <= sequenceOutput_.lastFrame; ++frame) {
                        core::ImageRgba8 image;
                        std::vector<std::uint64_t> sum;
                        for (std::uint32_t sample = 0; sample < sequenceOutput_.samples; ++sample) {
                            const float offset = sequenceOutput_.motionBlur
                                ? static_cast<float>(sample) / static_cast<float>(sequenceOutput_.samples) : 0.0F;
                            scene_.setFrame(static_cast<float>(frame) + offset);
                            animationFrame_ = scene_.timeline().frame;
                            refreshAnimatedMesh(false);
                            refreshPreviewScene();
                            auto rendered = device_->renderToImage({ videoWidth_, videoHeight_ });
                            if (sum.empty()) { image = rendered; sum.resize(rendered.pixels.size()); }
                            for (std::size_t i = 0; i < rendered.pixels.size(); ++i) sum[i] += rendered.pixels[i];
                        }
                        for (std::size_t i = 0; i < image.pixels.size(); ++i) image.pixels[i] = static_cast<std::uint8_t>(sum[i] / sequenceOutput_.samples);
                        output.push(frame, std::move(image));
                        if (frame == std::numeric_limits<std::uint32_t>::max()) break;
                    }
                    output.close();
                    output.rethrowIfFailed();
                    scene_.setFrame(restoreFrame);
                    animationFrame_ = restoreFrame;
                    sequenceOutputStatus_ = "Sequence rendered";
                } catch (const std::exception& error) {
                    scene_.setFrame(restoreFrame);
                    animationFrame_ = restoreFrame;
                    sequenceOutputStatus_ = error.what();
                }
            }
            ImGui::TextWrapped("%s", sequenceOutputStatus_.c_str());
            ImGui::TreePop();
        }
    }
    ImGui::End();

    if (ImGui::Begin("FX Debug")) {
        if (const auto* effect = scene_.effect()) {
            const auto compiled = core::compileEffectGraph(*effect);
            ImGui::Text("Resources: %zu", effect->textures.size());
            for (const auto& texture : effect->textures) ImGui::BulletText("%s (%s)", texture.name.c_str(), texture.format.c_str());
            ImGui::SeparatorText("Passes");
            for (const auto& pass : compiled.passes) {
                if (ImGui::TreeNode(pass.name.c_str())) {
                    ImGui::Text("Type: %s", core::toString(pass.type));
                    for (const auto& barrier : pass.barriers) ImGui::BulletText("Barrier: %s", barrier.c_str());
                    for (const auto& resource : pass.resources) ImGui::BulletText("%s %s", resource.write ? "Write" : "Read", resource.resource.c_str());
                    ImGui::TreePop();
                }
            }
        } else ImGui::TextUnformatted("Load an .fxdayo file to inspect resources and passes.");
    }
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
    ImGui::SetNextWindowPos({ 520.0F, 24.0F }, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({ 420.0F, 360.0F }, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Audio Export")) {
        if (!DAYO_HAS_MEDIA) {
            ImGui::TextUnformatted("FFmpeg support is not available in this build.");
        } else {
            ImGui::TextUnformatted("Format");
            ImGui::SameLine(120.0F);
            ImGui::TextUnformatted("M4A / AAC");
            ImGui::TextUnformatted("Source");
            ImGui::SameLine(120.0F);
            if (audioSource_.empty()) ImGui::TextUnformatted("Load an audio or video asset first");
            else ImGui::TextWrapped("%s", audioSource_.string().c_str());

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
                ImGui::ProgressBar(progress, { -1.0F, 0.0F });
                ImGui::Text("%.1f%%", static_cast<double>(progress) * 100.0);
                if (audioExportJob_.totalSeconds() > 0.0) {
                    ImGui::SameLine();
                    ImGui::Text("%.2f / %.2f s", audioExportJob_.processedSeconds(),
                                audioExportJob_.totalSeconds());
                }
                if (ImGui::Button("Cancel")) audioExportJob_.cancel();
            } else {
                const auto error = audioExportJob_.error();
                if (error) ImGui::TextWrapped("Export error: %s", error->c_str());
                const auto result = audioExportJob_.result();
                if (result) ImGui::TextWrapped("Exported: %s", result->output.string().c_str());
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
    }
    ImGui::End();
#endif
}

void Application::buildVideoExportUi() {
#if DAYO_HAS_IMGUI
    ImGui::SetNextWindowPos({ 520.0F, 404.0F }, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({ 420.0F, 410.0F }, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Video Export")) {
        if (!DAYO_HAS_MEDIA) {
            ImGui::TextUnformatted("FFmpeg support is not available in this build.");
        } else {
            if (!videoRangeInitialized_) {
                videoFromFrame_ = 0;
                videoToFrame_ = scene_.timeline().duration > 0.0F
                    ? static_cast<std::uint64_t>(std::ceil(scene_.timeline().duration)) : 0U;
                videoRangeInitialized_ = true;
            }
            if (videoDestination_[0] == '\0' && !audioSource_.empty()) {
                const auto destination = audioSource_.parent_path()
                    / (audioSource_.stem().string() + ".mp4");
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
            if (audioSource_.empty()) ImGui::TextUnformatted("No audio source (optional)");
            else ImGui::TextWrapped("%s", audioSource_.string().c_str());
            ImGui::InputText("Output", videoDestination_.data(), videoDestination_.size());
            int width = static_cast<int>(videoWidth_);
            int height = static_cast<int>(videoHeight_);
            if (ImGui::InputInt("Width", &width)) videoWidth_ = static_cast<std::uint32_t>(std::max(width, 1));
            if (ImGui::InputInt("Height", &height)) videoHeight_ = static_cast<std::uint32_t>(std::max(height, 1));
            ImGui::InputFloat("FPS", &videoFps_, 0.0F, 0.0F, "%.3f");
            videoFps_ = std::max(videoFps_, 1.0F);
            ImGui::Combo("Codec", &videoCodec_, "H.264\0H.265 / HEVC\0AV1\0");
            ImGui::SliderInt("Video bitrate (kbps)", &videoBitrateKbps_, 500, 50000);
            ImGui::SliderInt("Audio bitrate (kbps)", &videoAudioBitrateKbps_, 64, 512);
            ImGui::Checkbox("Include audio", &videoIncludeAudio_);
            auto from = static_cast<unsigned long long>(videoFromFrame_);
            auto to = static_cast<unsigned long long>(videoToFrame_);
            if (ImGui::InputScalar("From frame", ImGuiDataType_U64, &from)) videoFromFrame_ = from;
            if (ImGui::InputScalar("To frame", ImGuiDataType_U64, &to)) videoToFrame_ = to;
            ImGui::Checkbox("Overwrite existing file", &audioOverwrite_);
            ImGui::Separator();

            if (videoExportUiActive_ && videoExportJob_.running()) {
                ImGui::ProgressBar(videoExportJob_.progress(), { -1.0F, 0.0F });
                ImGui::Text("%.1f%%", static_cast<double>(videoExportJob_.progress()) * 100.0);
                if (ImGui::Button("Cancel video export")) {
                    videoExportJob_.cancel();
                    videoExportFramesFinished_ = true;
                    videoExportUiActive_ = false;
                    videoExportStatus_ = "Cancelled";
                }
            } else {
                if (!videoExportStatus_.empty()) ImGui::TextWrapped("%s", videoExportStatus_.c_str());
                if (const auto error = videoExportJob_.error()) {
                    ImGui::TextWrapped("Export error: %s", error->c_str());
                }
                if (const auto result = videoExportJob_.result()) {
                    ImGui::TextWrapped("Exported: %s (%.2f s)", result->output.string().c_str(),
                                       result->durationSeconds);
                }
                if (ImGui::Button("Export video")) {
                    try {
                        if (videoDestination_[0] == '\0') throw std::invalid_argument("video output is empty");
                        if (videoToFrame_ < videoFromFrame_) throw std::invalid_argument("video frame range is reversed");
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
                        if (request.includeAudio) audioSource = audioSource_;
                        videoSourceFps_ = sceneTimelineFps(scene_);
                        videoOutputFrameCount_ = videoOutputFrameCount(
                            videoFromFrame_, videoToFrame_, videoSourceFps_, request.fps);
                        videoExportJob_.start(std::move(request), std::move(audioSource),
                                              videoOutputFrameCount_);
                        videoNextFrame_ = 0;
                        videoPreviousSourceFrame_ = 0.0F;
                        videoPreRollDone_ = false;
                        videoExportFramesFinished_ = false;
                        videoExportUiActive_ = true;
                        videoExportStatus_.clear();
                        audioPlayer_.stop();
                    } catch (const std::exception& exception) {
                        videoExportStatus_ = std::string("Export error: ") + exception.what();
                    }
                }
            }
        }
    }
    ImGui::End();
#endif
}

} // namespace dayo::app
