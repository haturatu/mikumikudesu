#include "app/application.hpp"
#include "app/preview_conversion.hpp"
#include "app/preview_focus.hpp"
#include "app/viewport_navigation.hpp"
#include <deque>

#include "graphics/camera_matrices.hpp"
#include "graphics/fx_raster_semantics.hpp"
#include "graphics/preview_aperture.hpp"

#include "core/animation.hpp"
#include "core/asset.hpp"
#include "core/denoiser.hpp"
#include "core/fx/fx_controller_resolver.hpp"
#include "core/fx_debug.hpp"
#include "core/image.hpp"
#include "core/log.hpp"
#include "core/model_execution.hpp"
#include "core/model_probe.hpp"
#include "core/motion.hpp"
#include "core/video_export.hpp"
#include "core/vmdayo.hpp"
#include "graphics/device.hpp"
#include "platform/window.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

#include <SDL3/SDL.h>

#if DAYO_HAS_IMGUI
#include "ui/i18n_imgui.hpp"
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

#if DAYO_HAS_IMGUI
float halton(std::uint32_t index, std::uint32_t base) noexcept {
    float result = 0.0F;
    float fraction = 1.0F / static_cast<float>(base);
    while (index != 0) {
        result += static_cast<float>(index % base) * fraction;
        index /= base;
        fraction /= static_cast<float>(base);
    }
    return result;
}

void drawMorphWeightControl(float& value, const std::optional<core::fx::FxMorphControllerUi>& metadata) {
    const core::EffectSlider* slider =
        metadata.has_value() && metadata->slider.has_value() ? &*metadata->slider : nullptr;
    float minimum = slider != nullptr ? slider->minimum : 0.0F;
    float maximum = slider != nullptr ? slider->maximum : 1.0F;
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || maximum <= minimum) {
        minimum = 0.0F;
        maximum = 1.0F;
    }
    ImGuiSliderFlags flags = 0;
    if (slider != nullptr && slider->logarithmic)
        flags |= ImGuiSliderFlags_Logarithmic;

    bool changed = false;
    if (slider != nullptr && slider->integer) {
        const auto intMin = static_cast<int>(std::clamp(std::ceil(static_cast<double>(minimum)),
                                                        static_cast<double>(std::numeric_limits<int>::min()),
                                                        static_cast<double>(std::numeric_limits<int>::max())));
        const auto intMax = static_cast<int>(std::clamp(std::floor(static_cast<double>(maximum)),
                                                        static_cast<double>(std::numeric_limits<int>::min()),
                                                        static_cast<double>(std::numeric_limits<int>::max())));
        if (intMin <= intMax) {
            int integerValue = static_cast<int>(std::clamp(std::round(static_cast<double>(value)),
                                                           static_cast<double>(intMin), static_cast<double>(intMax)));
            changed = ImGui::SliderInt(ui::label("Weight"), &integerValue, intMin, intMax, "%d", flags);
            value = static_cast<float>(integerValue);
        } else {
            changed = ImGui::SliderFloat(ui::label("Weight"), &value, minimum, maximum, "%.3f", flags);
        }
    } else {
        changed = ImGui::SliderFloat(ui::label("Weight"), &value, minimum, maximum, "%.3f", flags);
    }

    if (changed && slider != nullptr && slider->step > 0.0F && std::isfinite(slider->step)) {
        value = minimum + std::round((value - minimum) / slider->step) * slider->step;
        value = std::clamp(value, minimum, maximum);
        if (slider->integer)
            value = std::round(value);
    }

    if (metadata.has_value() && !metadata->descriptions.empty() && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        for (const auto& description : metadata->descriptions)
            ImGui::TextWrapped("%s", description.c_str());
        ImGui::EndTooltip();
    }
    if (slider != nullptr && ImGui::BeginPopupContextItem("morph-weight-options")) {
        if (ImGui::MenuItem(ui::label("Reset to controller default")))
            value = std::clamp(slider->defaultValue, minimum, maximum);
        ImGui::EndPopup();
    }
}
#endif

double sceneTimelineFps(const core::Scene& scene) noexcept {
    const auto value = static_cast<double>(scene.timeline().fps);
    return std::isfinite(value) && value > 0.0 ? value : 30.0;
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
    if (texture.width == 0 || texture.height == 0)
        return false;
    const auto width = static_cast<std::size_t>(texture.width);
    const auto height = static_cast<std::size_t>(texture.height);
    if (width > std::numeric_limits<std::size_t>::max() / height)
        return false;
    const auto pixelCount = width * height;
    return pixelCount <= std::numeric_limits<std::size_t>::max() / 4U && texture.pixels.size() == pixelCount * 4U;
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

std::uint64_t environmentFileVersion(const std::filesystem::path& path) noexcept {
    std::error_code error;
    const auto timestamp = std::filesystem::last_write_time(path, error);
    if (error)
        return 0;
    const auto fileSize = std::filesystem::file_size(path, error);
    if (error)
        return static_cast<std::uint64_t>(timestamp.time_since_epoch().count());
    const auto ticks = static_cast<std::uint64_t>(timestamp.time_since_epoch().count());
    return ticks ^ (fileSize + 0x9e3779b97f4a7c15ULL + (ticks << 6U) + (ticks >> 2U));
}

bool hasEffectMemo(const core::SceneEffectStack& effects, const fx::FxProgram* rendererProgram,
                   std::string_view requested) {
    const auto equalMemo = [requested](std::string_view memo) {
        return memo.size() == requested.size() &&
               std::equal(memo.begin(), memo.end(), requested.begin(), [](unsigned char left, unsigned char right) {
                   return std::tolower(left) == std::tolower(right);
               });
    };
    const auto matches = [&equalMemo](const core::EffectGraph& graph) {
        return std::ranges::any_of(graph.memos, [&](const std::string& memo) { return equalMemo(memo); });
    };
    if (std::ranges::any_of(effects.deform, [&](const auto& effect) { return matches(effect.graph); }) ||
        std::ranges::any_of(effects.postprocess, [&](const auto& effect) { return matches(effect.graph); }) ||
        (effects.renderer.has_value() && matches(effects.renderer->graph)))
        return true;
    if (rendererProgram == nullptr)
        return false;
    return std::ranges::any_of(rendererProgram->memos, [&](const std::string& memo) { return equalMemo(memo); });
}

std::optional<std::filesystem::path> findDayoHlslDirectory(const core::SceneEffectStack& effects,
                                                           const fx::FxProgram* rendererProgram) {
    std::vector<std::filesystem::path> sources;
    if (rendererProgram != nullptr && !rendererProgram->sourcePath.empty())
        sources.push_back(rendererProgram->sourcePath);
    const auto appendSources = [&sources](const auto& instances) {
        for (const auto& instance : instances) {
            if (!instance.source.empty())
                sources.push_back(instance.source);
            if (!instance.graph.sourcePath.empty())
                sources.push_back(instance.graph.sourcePath);
        }
    };
    appendSources(effects.deform);
    appendSources(effects.postprocess);
    if (effects.renderer.has_value()) {
        sources.push_back(effects.renderer->source);
        sources.push_back(effects.renderer->graph.sourcePath);
    }

    std::error_code error;
    for (const auto& source : sources) {
        if (source.empty())
            continue;
        auto directory = source.parent_path();
        while (!directory.empty()) {
            const auto hlsl = directory / "hlsl";
            error.clear();
            const bool hasPdf = std::filesystem::is_regular_file(hlsl / "system" / "skyboxPDF.hlsl", error);
            error.clear();
            const bool hasSh = std::filesystem::is_regular_file(hlsl / "system" / "skyboxSH.hlsl", error);
            if (hasPdf && hasSh)
                return std::filesystem::absolute(hlsl).lexically_normal();
            const auto parent = directory.parent_path();
            if (parent == directory)
                break;
            directory = parent;
        }
    }
    return std::nullopt;
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

bool Application::ensureNativeSceneRuntime(bool restartRenderer, std::string* error) {
    if (error != nullptr)
        error->clear();
    if (device_ == nullptr || scene_.effect() == nullptr) {
        if (error != nullptr)
            *error = "native scene runtime requires a device and effect";
        return false;
    }
    if (nativeSceneModelData_.size() > std::numeric_limits<std::uint32_t>::max()) {
        if (error != nullptr)
            *error = "native scene model count exceeds descriptor limits";
        return false;
    }
    if (textures_.size() > std::numeric_limits<std::uint32_t>::max()) {
        if (error != nullptr)
            *error = "native scene texture count exceeds descriptor limits";
        return false;
    }
    std::vector<core::EffectController> controllerDeclarations;
    if (scene_.effects().renderer.has_value())
        controllerDeclarations = scene_.effects().renderer->graph.controllers;
    const auto sameControllers = [](std::span<const core::EffectController> left,
                                    std::span<const core::EffectController> right) {
        if (left.size() != right.size())
            return false;
        return std::ranges::equal(left, right, [](const auto& lhs, const auto& rhs) {
            return lhs.name == rhs.name && lhs.controllerName == rhs.controllerName && lhs.item == rhs.item &&
                   lhs.type == rhs.type;
        });
    };
    nativeRenderer_.setControllerDeclarations(controllerDeclarations);
    const auto modelCount = static_cast<std::uint32_t>(std::max<std::size_t>(1, nativeSceneModelData_.size()));
    const graphics::NativeSceneDescriptorCounts counts{
        .textures = static_cast<std::uint32_t>(std::max<std::size_t>(1, textures_.size())),
        .vertexBuffers = modelCount,
        .indexBuffers = modelCount,
        .materials = modelCount,
        .faces = modelCount,
        .materialFaces = modelCount,
        .faceWalkers = modelCount,
        .previousVertices = modelCount,
        .rawVertices = modelCount};
    const auto sameCounts = [](const auto& left, const auto& right) {
        return left.textures == right.textures && left.vertexBuffers == right.vertexBuffers &&
               left.indexBuffers == right.indexBuffers && left.materials == right.materials &&
               left.faces == right.faces && left.materialFaces == right.materialFaces &&
               left.faceWalkers == right.faceWalkers && left.previousVertices == right.previousVertices &&
               left.rawVertices == right.rawVertices;
    };
    if (nativeSceneFrame_.ready() && nativeSceneResources_.ready() &&
        sameCounts(nativeSceneFrame_.resources().counts(), counts) &&
        sameCounts(nativeSceneResources_.counts(), counts) &&
        sameControllers(nativeControllerDeclarations_, controllerDeclarations)) {
        if (!nativeSceneDerivedRuntime_.ready() && !nativeSceneDerivedRuntime_.initialize(*device_, textures_, error))
            return false;
        nativeRenderer_.setSceneFrameRuntime(&nativeSceneFrame_);
        return true;
    }

    const bool wasNative = nativeRenderer_.status().nativeReady;
    nativeSceneFrame_.reset();
    nativeSceneResources_.reset();
    nativeSceneDerivedRuntime_.reset();
    nativeSceneDraws_.clear();
    nativeEffectModels_.clear();
    try {
        std::string runtimeError;
        if (!nativeSceneResources_.initialize(*device_, counts, &runtimeError))
            throw std::runtime_error(runtimeError.empty() ? "native scene resource store initialization failed"
                                                          : runtimeError);
        if (!nativeSceneDerivedRuntime_.initialize(*device_, textures_, &runtimeError))
            throw std::runtime_error(runtimeError.empty() ? "native scene derived runtime initialization failed"
                                                          : runtimeError);
        if (!nativeSceneFrame_.initialize(*device_, controllerDeclarations, counts, &runtimeError))
            throw std::runtime_error(runtimeError.empty() ? "native scene frame initialization failed" : runtimeError);
        nativeControllerDeclarations_ = controllerDeclarations;
        nativeRenderer_.setSceneFrameRuntime(&nativeSceneFrame_);
    } catch (const std::exception& exception) {
        nativeSceneFrame_.reset();
        nativeSceneResources_.reset();
        nativeSceneDerivedRuntime_.reset();
        if (error != nullptr)
            *error = exception.what();
        return false;
    } catch (...) {
        nativeSceneFrame_.reset();
        nativeSceneResources_.reset();
        nativeSceneDerivedRuntime_.reset();
        if (error != nullptr)
            *error = "native scene runtime initialization failed";
        return false;
    }

    if (restartRenderer && wasNative) {
        nativeRenderer_.reset();
        requestRenderer(requestedRenderer_);
        if (!nativeRenderer_.status().nativeReady) {
            if (error != nullptr)
                *error = nativeRenderer_.status().reason;
            return false;
        }
    }
    return true;
}

void Application::requestRenderer(graphics::RendererKind renderer) {
    requestedRenderer_ = renderer;
    if (device_ == nullptr)
        return;
    if (scene_.effect() == nullptr) {
        nativeRenderer_.reset();
        device_->setNativeRendererAvailability(false, false);
        device_->selectRenderer(renderer);
        return;
    }
    try {
        if (renderer != graphics::RendererKind::preview) {
            std::string sceneError;
            if (!ensureNativeSceneRuntime(false, &sceneError))
                throw std::runtime_error(sceneError.empty() ? "native scene runtime unavailable" : sceneError);
            nativeRenderer_.setSceneFrameRuntime(&nativeSceneFrame_);
        }
        const auto status = nativeRenderer_.prepare(*device_, renderer, *scene_.effect());
        const auto& sourceEffects = scene_.effects();
        auto runtimeEffects = sourceEffects;
        std::erase_if(runtimeEffects.deform, [this, &sourceEffects](const auto& effect) {
            if (!effect.controllerModel.has_value())
                return false;
            const auto* owner = scene_.model(*effect.controllerModel);
            return owner != nullptr && !core::ModelExecutionPlanner::resolve(*owner, sourceEffects).runDeformer;
        });
        nativeRenderer_.setEffectStack(runtimeEffects);
        device_->setNativeRendererAvailability(status.nativeReady && status.active == graphics::RendererKind::subayai,
                                               status.nativeReady && status.active == graphics::RendererKind::bdpt);
        device_->selectRenderer(status.active);
        if (status.fellBack())
            log::warn("Native ", graphics::toString(renderer), " unavailable; using Preview: ", status.reason);
    } catch (const std::exception& exception) {
        nativeRenderer_.reset();
        device_->setNativeRendererAvailability(false, false);
        device_->selectRenderer(graphics::RendererKind::preview);
        log::warn("Native ", graphics::toString(renderer),
                  " effect preparation failed; using Preview: ", exception.what());
    }
}

fx::FxCameraState Application::makeSceneCameraState() const {
    const auto* model = selectedModel();
    const auto* globalCameraMotion = scene_.cameraMotion();
    const auto* motion = globalCameraMotion != nullptr && !globalCameraMotion->cameras.empty()
                             ? globalCameraMotion
                             : (model != nullptr ? model->motion.get() : nullptr);
    fx::FxCameraState camera;
    camera.rotation = {cameraPitch_, cameraYaw_, 0.0F};
    camera.distance = cameraDistance_;
    camera.position = cameraTarget_;
    camera.verticalFovRadians = cameraFovRadians_;
    camera.perspective = cameraPerspective_;
#if DAYO_HAS_IMGUI
    if (cameraEditPreview_ && !manualCamera_) {
        const auto normalization = model != nullptr ? model->normalization : normalization_;
        for (std::size_t axis = 0; axis < 3; ++axis)
            camera.position[axis] = (editedCamera_.position[axis] - normalization.center[axis]) * normalization.scale;
        camera.rotation = editedCamera_.rotation;
        camera.distance = std::max(std::abs(editedCamera_.distance) * normalization.scale, 0.4F);
        camera.verticalFovRadians = std::clamp(editedCamera_.viewAngle, 1.0F, 179.0F) * 0.01745329252F;
        camera.perspective = editedCamera_.perspective;
        return camera;
    }
#endif
    if (!manualCamera_ && motion != nullptr && !motion->cameras.empty()) {
        const auto evaluated = core::evaluateCamera(*motion, animationFrame_);
        const auto normalization = model != nullptr ? model->normalization : normalization_;
        for (std::size_t axis = 0; axis < 3; ++axis)
            camera.position[axis] = (evaluated.position[axis] - normalization.center[axis]) * normalization.scale;
        camera.rotation = evaluated.rotation;
        camera.distance = std::max(std::abs(evaluated.distance) * normalization.scale, 0.4F);
        camera.verticalFovRadians = std::clamp(evaluated.viewAngle, 1.0F, 179.0F) * 0.01745329252F;
        camera.perspective = evaluated.perspective;
    }
    return camera;
}

fx::FxFrameContext Application::makeNativeFrameContext(const graphics::RenderTargetDesc& target,
                                                       const fx::FxHostFrameState& invocationEvents) {
    const auto* model = selectedModel();
    const auto* globalLightMotion = scene_.cameraMotion();
    const auto* motion = globalLightMotion != nullptr && !globalLightMotion->lights.empty()
                             ? globalLightMotion
                             : (model != nullptr ? model->motion.get() : nullptr);
    const auto camera = makeSceneCameraState();
    const auto matrices =
        graphics::makeSceneCameraMatrices(camera, target.width, target.height,
                                          device_ == nullptr ? graphics::GraphicsConvention{} : device_->convention());
    auto cameraWithMatrices = camera;
    cameraWithMatrices.view = matrices.view;
    cameraWithMatrices.projection = matrices.projection;
    cameraWithMatrices.viewProjection = matrices.viewProjection;
    fx::FxLightingState lighting;
    if (motion != nullptr && !motion->lights.empty()) {
        const auto evaluated = core::evaluateLight(*motion, animationFrame_);
        lighting.direction = evaluated.position;
        lighting.color = evaluated.color;
    }
#if DAYO_HAS_IMGUI
    if (lightEditPreview_) {
        lighting.direction = editedLight_.position;
        lighting.color = editedLight_.color;
    }
#endif
    std::uint32_t modelIndex = 0;
    std::size_t totalMaterials = 0;
    for (std::size_t index = 0; index < scene_.models().size(); ++index) {
        const auto& instance = scene_.models()[index];
        if (instance.id == scene_.selectedModelId())
            modelIndex = static_cast<std::uint32_t>(index);
        if (core::ModelExecutionPlanner::resolve(instance, scene_.effects()).rasterize && instance.model != nullptr)
            totalMaterials += instance.model->materials.size();
    }
    const auto* program = nativeRenderer_.program();
    const auto sceneCloneCount = model == nullptr ? 1U : model->cloneCount;
    const auto effectCloneCount = program == nullptr ? 1U : program->meshCloneCount;
    auto context =
        fx::makeFxFrameContext(animationFrame_, scene_.accumulatedSamples(), target.width, target.height,
                               model == nullptr ? 0U : model->id, modelIndex, animatedVertexCount_, totalMaterials,
                               sceneCloneCount, effectCloneCount, cameraWithMatrices, lighting);
    context.modelCount = static_cast<std::uint32_t>(nativeSceneModelData_.size());
    const auto& background = scene_.background();
    context.host.backgroundTransparent = background.mode == core::BackgroundMode::alpha;
    context.host.screenBmpMode =
        !background.enabled || background.screenSource == core::ScreenTextureSource::white ? 0 : 1;
    context.host.backgroundMode =
        !background.enabled || background.screenSource == core::ScreenTextureSource::white ? 2 : 1;
    context.host.playing = playing_;
    context.host.onStart = invocationEvents.onStart;
    context.host.onResize = invocationEvents.onResize;
    context.host.onModelChanged = invocationEvents.onModelChanged;
    context.host.onMaterialChanged = invocationEvents.onMaterialChanged;
    return context;
}

std::optional<graphics::NativeFrameOutput> Application::recordNativeFrame(graphics::CommandList& commands,
                                                                          const graphics::RenderTargetDesc& target) {
    if (device_ == nullptr)
        return std::nullopt;
    if (device_->activeRenderer() == graphics::RendererKind::preview) {
        const auto& background = scene_.background();
        std::filesystem::path lightingSource = projectEditorState_.skyboxFile;
        if (lightingSource.empty() && background.image && background.imagePath &&
            static_cast<std::uint64_t>(background.image->height) * 2U == background.image->width)
            lightingSource = *background.imagePath;
        if (!lightingSource.empty()) {
            const auto version = environmentFileVersion(lightingSource);
            if (lightingSource == failedPreviewHdriPath_ && version == failedPreviewHdriVersion_) {
                device_->updatePreviewEnvironment({});
            } else {
                try {
                    static_cast<void>(nativeRenderer_.updateEnvironment(
                        {.source = lightingSource.string(), .exposure = 1.0F, .version = version}));
                    nativeRenderer_.recordEnvironment(commands);
                    const auto& environment = nativeRenderer_.environment();
                    device_->updatePreviewEnvironment({.prefiltered = environment.prefiltered,
                                                       .sphericalHarmonics = environment.sphericalHarmonics,
                                                       .mipLevels = environment.prefilteredMipLevels});
                    previewHdriError_.clear();
                    failedPreviewHdriPath_.clear();
                } catch (const std::exception& error) {
                    failedPreviewHdriPath_ = lightingSource;
                    failedPreviewHdriVersion_ = version;
                    previewHdriError_ = error.what();
                    log::warn("Preview HDRI: ", previewHdriError_);
                    nativeRenderer_.clearEnvironment();
                    device_->updatePreviewEnvironment({});
                }
            }
        } else {
            nativeRenderer_.clearEnvironment();
            device_->updatePreviewEnvironment({});
            previewHdriError_.clear();
        }
        return std::nullopt;
    }
    graphics::NativeFrameExecution outputExecution;
#if DAYO_HAS_IMGUI
    if (imageSequenceExportRunning_) {
        outputExecution.sampleIndex = imageSequenceSampleIndex_;
        outputExecution.sampleCount = imageSequenceSampleCount_;
    }
#endif
    nativeFxPendingEvents_.latch(scene_.dirty(core::DirtyFlag::geometry), scene_.dirty(core::DirtyFlag::material));
    const auto& background = scene_.background();
    const bool buildSkyboxPrefilter = hasEffectMemo(scene_.effects(), nativeRenderer_.program(), "skyboxprefilter");
    if (background.image && background.imagePath &&
        static_cast<std::uint64_t>(background.image->height) * 2U == background.image->width) {
        const auto sourceVersion = environmentFileVersion(*background.imagePath);
        static_cast<void>(nativeRenderer_.updateEnvironment(
            {.source = background.imagePath->string(), .exposure = 1.0F, .version = sourceVersion}));
        const auto& environment = nativeRenderer_.environment();
        if (environment.skybox.valid()) {
            const auto hlslDirectory = findDayoHlslDirectory(scene_.effects(), nativeRenderer_.program());
            if (!hlslDirectory.has_value())
                throw std::runtime_error("cannot locate the pinned MikuMikuDayo hlsl/system environment shaders for "
                                         "the active effect stack");
            std::string environmentError;
            const bool buildSkyboxSampler = hasEffectMemo(scene_.effects(), nativeRenderer_.program(), "skyboxsampler");
            const graphics::Extent3D extent{background.image->width, background.image->height, 1};
            if (!nativeDayoEnvironmentRuntime_.sync(*device_, commands, environment.skybox, extent,
                                                    *background.imagePath, sourceVersion, *hlslDirectory,
                                                    buildSkyboxSampler, &environmentError, buildSkyboxPrefilter))
                throw std::runtime_error(environmentError.empty() ? "canonical Dayo environment sync failed"
                                                                  : environmentError);
        } else {
            nativeDayoEnvironmentRuntime_.reset();
            nativeRenderer_.clearEnvironment();
        }
    } else {
        nativeDayoEnvironmentRuntime_.reset();
        nativeRenderer_.clearEnvironment();
    }
    std::vector<core::MaterialParameterBlock> materials;
    for (const auto& instance : scene_.models()) {
        if (instance.model == nullptr || !core::ModelExecutionPlanner::resolve(instance, scene_.effects()).rasterize)
            continue;
        for (std::size_t index = 0; index < instance.model->materials.size(); ++index) {
            if (index < instance.materialSettings.size())
                materials.push_back(instance.materialSettings[index].parameters);
            else
                materials.emplace_back();
        }
    }
    const auto nativeDirty = scene_.dirtyFlags();
    if (nativeLightSampling_.lightCount() == 0 || scene_.dirty(core::DirtyFlag::lighting)) {
        float power = 1.0F;
        const auto* lightingMotion = scene_.cameraMotion();
        if (lightingMotion != nullptr && !lightingMotion->lights.empty()) {
            const auto light = core::evaluateLight(*lightingMotion, animationFrame_);
            power = std::max(0.0F, 0.2126F * light.color[0] + 0.7152F * light.color[1] + 0.0722F * light.color[2]);
        }
        if (!std::isfinite(power) || power <= 0.0F)
            power = 1.0F;
        nativeLightPowers_ = {power};
        nativeLightSampling_.update(nativeLightPowers_, true);
        scene_.clearDirty(core::DirtyFlag::lighting);
    }
    const auto lightSampling = nativeLightSampling_.table();
    scheduledEffects_ = effectScheduler_.schedule(
        scene_.effects(), [this](core::ModelId id) -> std::optional<core::ModelExecutionOrder> {
            const auto* model = scene_.model(id);
            return model == nullptr ? std::nullopt : std::optional<core::ModelExecutionOrder>{model->order};
        });
    nativeRenderer_.setEffectSchedule(scheduledEffects_);
    try {
        std::string modelError;
        if (!nativeSceneModelData_.empty() &&
            !nativeSceneModelRuntime_.updateFrame(*device_, commands, nativeSceneModelData_, &modelError))
            throw std::runtime_error(modelError.empty() ? "native scene model synchronization failed" : modelError);
        std::string sceneError;
        if (!ensureNativeSceneRuntime(true, &sceneError))
            throw std::runtime_error(sceneError.empty() ? "native scene runtime synchronization failed" : sceneError);
        const graphics::Extent3D screenExtent{target.width, target.height, 1};
        const bool nativeResizeEvent =
            !nativeScreenRuntime_.ready() || !nativeScreenRuntime_.matchesExtent(screenExtent);
        if (nativeResizeEvent) {
            if (!nativeScreenRuntime_.initialize(*device_, screenExtent, &sceneError))
                throw std::runtime_error(sceneError.empty() ? "native screen runtime initialization failed"
                                                            : sceneError);
        }
        const auto& backgroundState = scene_.background();
        auto screenSource = graphics::NativeScreenSource::previousFrame;
        std::optional<core::ImageRgba8> nativeBackground;
        if (!backgroundState.enabled || backgroundState.screenSource == core::ScreenTextureSource::white) {
            screenSource = graphics::NativeScreenSource::white;
        } else if (backgroundState.screenSource == core::ScreenTextureSource::backgroundImage &&
                   backgroundState.image.has_value()) {
            nativeBackground = backgroundState.image;
            screenSource = graphics::NativeScreenSource::external;
        } else if (videoMode_ && videoVisible_ &&
                   backgroundState.screenSource == core::ScreenTextureSource::backgroundVideo) {
            auto* media = scene_.backgroundMedia();
            if (media != nullptr && media->info().hasVideo)
                nativeBackground = media->decodeVideoFrame(backgroundVideoSeconds());
            if (nativeBackground.has_value())
                screenSource = graphics::NativeScreenSource::external;
        }
        if (nativeBackground.has_value()) {
            const auto crop = backgroundState.crop == core::ScreenCropMode::crop4x3
                                  ? graphics::NativeScreenCrop::crop4x3
                                  : graphics::NativeScreenCrop::none;
            if (!nativeScreenRuntime_.uploadScreenBmp(*nativeBackground, crop, &sceneError))
                throw std::runtime_error(sceneError.empty() ? "native ScreenBMP upload failed" : sceneError);
        }
        nativeScreenRuntime_.prepareFrame(commands, screenSource, backgroundState.enabled,
                                          backgroundState.crop == core::ScreenCropMode::crop4x3
                                              ? graphics::NativeScreenCrop::crop4x3
                                              : graphics::NativeScreenCrop::none);
        std::vector<graphics::NativeGeometryMeshUpload> geometryUploads;
        std::vector<graphics::WorldInstance> worldInstances;
        graphics::handles::AccelerationStructureHandle tlas;
        geometryUploads.reserve(nativeGeometry_.size());
        for (const auto& mesh : nativeGeometry_) {
            if (!mesh.hasBlas)
                continue;
            geometryUploads.push_back({
                .meshId = mesh.meshId,
                .deform = {.baseVertices = mesh.baseVertices,
                           .bones = mesh.bones,
                           .morphDeltas = mesh.morphDeltas,
                           .morphWeights = mesh.morphWeights,
                           .indices = mesh.indices,
                           .deformedVertices = mesh.deformedVertices},
                .deformPipeline = device_->nativeDeformPipeline(),
                .deformDescriptorLayout = device_->nativeDeformDescriptorLayout(),
                .topologyGeneration = scene_.topologyGeneration(),
                .deformVersion = nativeDeformVersion_,
            });
            if (!mesh.acceleration)
                continue;
            const auto cloneCenter = (static_cast<float>(mesh.cloneCount) - 1.0F) * 0.5F;
            for (std::uint32_t clone = 0; clone < mesh.cloneCount; ++clone) {
                graphics::Matrix3x4 transform;
                transform.values[3] = (static_cast<float>(clone) - cloneCenter) * 2.2F;
                worldInstances.push_back({.meshId = mesh.meshId, .transform = transform});
            }
        }
        const auto synchronizeGeometry = [&](auto* runtime) {
            std::string geometryError;
            if (!runtime->syncGeometry(geometryUploads, &geometryError))
                throw std::runtime_error(geometryError.empty() ? "native geometry synchronization failed"
                                                               : geometryError);
            if (geometryUploads.empty())
                return;
            if (!runtime->synchronizeAcceleration(&geometryError))
                throw std::runtime_error(geometryError.empty() ? "native acceleration synchronization failed"
                                                               : geometryError);
            static_cast<void>(runtime->synchronizeWorld(nativeDeformVersion_, worldInstances));
            tlas = runtime->geometry().tlas();
            runtime->recordGeometry(commands, geometryUploads);
            runtime->recordAcceleration(commands);
        };
        if (auto* runtime = nativeRenderer_.subayai())
            synchronizeGeometry(runtime);
        else if (auto* bdptRuntime = nativeRenderer_.bdpt())
            synchronizeGeometry(bdptRuntime);
        if (!tlas.valid())
            throw std::runtime_error("native scene runtime requires a synchronized TLAS");
        auto frameContext =
            makeNativeFrameContext(target, {.onStart = nativeOnStartPending_,
                                            .onResize = nativeResizeEvent,
                                            .onModelChanged = nativeFxPendingEvents_.modelChanged,
                                            .onMaterialChanged = nativeFxPendingEvents_.materialChanged});
        if (outputExecution.sampleCount > 1) {
            frameContext.sample = outputExecution.sampleIndex;
            frameContext.sampleCount = outputExecution.sampleCount;
        }
        graphics::populateNativeViewExpressionSymbols(
            frameContext,
            graphics::makeNativeViewConstants(frameContext, static_cast<std::uint32_t>(frameContext.totalMaterial)));
        auto sceneResources = nativeSceneResources_.bindings();
        const auto modelResources = nativeSceneModelRuntime_.bindings();
        std::vector<graphics::NativeSceneDerivedModel> derivedModels;
        derivedModels.reserve(nativeSceneModelData_.size());
        for (std::size_t modelIndex = 0; modelIndex < nativeSceneModelData_.size(); ++modelIndex) {
            if (modelIndex >= nativeGeometry_.size())
                throw std::runtime_error("native scene derived model table is out of sync with geometry");
            if (nativeSceneModelData_[modelIndex].materials.size() > std::numeric_limits<std::uint32_t>::max())
                throw std::overflow_error("native scene model material count exceeds 32-bit table indices");
            const auto& geometry = nativeGeometry_[modelIndex];
            const auto* instance = scene_.model(geometry.modelId);
            std::int32_t selectedMaterial = -1;
#if DAYO_HAS_IMGUI
            if (const auto* selected = scene_.selectedModel(); selected != nullptr && selected->id == geometry.modelId)
                selectedMaterial = uiState_.selectedMaterial;
#endif
            derivedModels.push_back(
                {.materialCount = static_cast<std::uint32_t>(nativeSceneModelData_[modelIndex].materials.size()),
                 .textureBase = geometry.textureBase,
                 .cloneCount = geometry.cloneCount,
                 .selectedMaterial = selectedMaterial,
                 .visible = geometry.rasterize && instance != nullptr});
        }
        if (!nativeSceneDerivedRuntime_.sync(derivedModels, screenExtent, &sceneError))
            throw std::runtime_error(sceneError.empty() ? "native scene derived resource synchronization failed"
                                                        : sceneError);
        nativeSceneDerivedRuntime_.apply(sceneResources);
        nativeDayoEnvironmentRuntime_.apply(sceneResources);
        sceneResources.tlas = tlas;
        sceneResources.vertexBuffers = modelResources.vertexBuffers;
        sceneResources.indexBuffers = modelResources.indexBuffers;
        sceneResources.materials = modelResources.materials;
        sceneResources.faces = modelResources.faces;
        sceneResources.materialFaces = modelResources.materialFaces;
        sceneResources.faceWalkers = modelResources.faceWalkers;
        sceneResources.previousVertices = modelResources.previousVertices;
        sceneResources.rawVertices = modelResources.rawVertices;
        nativeScreenRuntime_.bindScreenSemantics(sceneResources);
        if (!nativeSceneResources_.compose(sceneResources, &sceneError))
            throw std::runtime_error(sceneError.empty() ? "native scene resource composition failed" : sceneError);
        nativeSceneDraws_.clear();
        nativeEffectModels_.clear();
        for (std::size_t modelIndex = 0; modelIndex < nativeSceneModelData_.size(); ++modelIndex) {
            if (modelIndex >= nativeGeometry_.size() || modelIndex >= modelResources.vertexBuffers.size() ||
                modelIndex >= modelResources.indexBuffers.size())
                throw std::runtime_error("native scene draw table is out of sync with model resources");
            const auto& model = nativeSceneModelData_[modelIndex];
            const auto& geometry = nativeGeometry_[modelIndex];
            nativeEffectModels_.push_back({.modelId = geometry.modelId,
                                           .modelIndex = geometry.modelIndex,
                                           .vertexCount = geometry.baseVertices.size(),
                                           .materialCount = model.materials.size(),
                                           .cloneCount = geometry.cloneCount,
                                           .rasterizeOrder = geometry.rasterizeOrder,
                                           .deformIndex = geometry.deformIndex,
                                           .deformOrder = geometry.deformOrder});
            if (!geometry.rasterize)
                continue;
            for (std::size_t materialIndex = 0; materialIndex < model.materialFaces.size(); ++materialIndex) {
                const auto& range = model.materialFaces[materialIndex];
                if (range.count == 0)
                    continue;
                const auto firstIndex = static_cast<std::uint64_t>(range.start) * 3U;
                const auto indexCount = static_cast<std::uint64_t>(range.count) * 3U;
                if (firstIndex > std::numeric_limits<std::uint32_t>::max() ||
                    indexCount > std::numeric_limits<std::uint32_t>::max())
                    throw std::overflow_error("native scene draw range exceeds indexed draw limits");
                nativeSceneDraws_.push_back({.vertexBuffer = modelResources.vertexBuffers[modelIndex],
                                             .indexBuffer = modelResources.indexBuffers[modelIndex],
                                             .modelIndex = geometry.modelIndex,
                                             .materialIndex = static_cast<std::uint32_t>(materialIndex),
                                             .firstIndex = static_cast<std::uint32_t>(firstIndex),
                                             .indexCount = static_cast<std::uint32_t>(indexCount),
                                             .instanceCount = geometry.cloneCount,
                                             .firstInstance = 0,
                                             .vertexOffset = 0,
                                             .buffer = false,
                                             .rasterizeOrder = geometry.rasterizeOrder,
                                             .deformIndex = geometry.deformIndex,
                                             .deformOrder = geometry.deformOrder});
            }
        }
        if (!nativeSceneFrame_.sync(frameContext, nativeSceneResources_.bindings(), {}, &sceneError))
            throw std::runtime_error(sceneError.empty() ? "native scene frame synchronization failed" : sceneError);
        if (const auto* program = nativeRenderer_.program();
            program != nullptr &&
            !nativeSceneFrame_.syncControllers(program->controllers, evaluatedModels_,
                                               scene_.effects().renderer.has_value() &&
                                                       scene_.effects().renderer->controllerModel.has_value()
                                                   ? *scene_.effects().renderer->controllerModel
                                                   : 0,
                                               &sceneError))
            throw std::runtime_error(sceneError.empty() ? "native FX controller synchronization failed" : sceneError);
        auto hostBindings = nativeSceneResources_.bindings();
        hostBindings.viewConstants = nativeSceneFrame_.constants().viewBuffer();
        hostBindings.controllerConstants = nativeSceneFrame_.controllers().buffer();
        hostBindings.passConstants = nativeSceneFrame_.constants().passBuffer();
        hostBindings.hostResourceMask |= graphics::dayoSemanticBit(graphics::DayoSemantic::ViewCB) |
                                         graphics::dayoSemanticBit(graphics::DayoSemantic::ControllerCB) |
                                         graphics::dayoSemanticBit(graphics::DayoSemantic::CBuff1);
        nativeRenderer_.setHostResourceBindings(hostBindings);
        graphics::FxExecutionResources executionResources;
        executionResources.sceneDraws = nativeSceneDraws_;
        executionResources.effectModels = nativeEffectModels_;
        const auto controllerModel =
            scene_.effects().renderer.has_value() && scene_.effects().renderer->controllerModel.has_value()
                ? *scene_.effects().renderer->controllerModel
                : core::ModelId{};
        for (const auto& geometry : nativeGeometry_) {
            if (geometry.modelId == controllerModel) {
                executionResources.rasterControllerModel = geometry.modelIndex;
                break;
            }
        }
        executionResources.updatePassConstants = [this](graphics::CommandList& commandList,
                                                        const graphics::NativeSceneDraw& draw) {
            const graphics::NativeScenePassConstants pass{.modelIndex = draw.modelIndex,
                                                          .rasterizeOrder = draw.rasterizeOrder,
                                                          .deformIndex = draw.deformIndex,
                                                          .deformOrder = draw.deformOrder};
            std::string passError;
            if (!nativeSceneFrame_.updatePassConstants(commandList, pass, &passError))
                throw std::runtime_error(passError.empty() ? "native CBuff1 update failed" : passError);
        };
        executionResources.updateEffectPassConstants = [this](graphics::CommandList& commandList,
                                                              const graphics::NativeEffectModel& model) {
            const graphics::NativeScenePassConstants pass{.modelIndex = model.modelIndex,
                                                          .rasterizeOrder = model.rasterizeOrder,
                                                          .deformIndex = model.deformIndex,
                                                          .deformOrder = model.deformOrder};
            std::string passError;
            if (!nativeSceneFrame_.updatePassConstants(commandList, pass, &passError))
                throw std::runtime_error(passError.empty() ? "native deform CBuff1 update failed" : passError);
        };
        executionResources.executeOidnWithResolver =
            [this](const fx::FxOidnDispatch& dispatch, const fx::FxFrameContext& context,
                   graphics::CommandList& commandList,
                   const graphics::FxExecutionResources::TypedResourceResolver& resolve) {
                std::string oidnError;
                if (nativeOidnProvider_.execute(dispatch, context, commandList, resolve, &oidnError))
                    return true;
                log::warn("Native OIDN pass failed: ", oidnError);
                return false;
            };
        std::vector<graphics::FxMaterialSceneModel> materialModels;
        const auto* activeProgram = nativeRenderer_.program();
        if (activeProgram != nullptr && activeProgram->materialSchema.has_value()) {
            if (nativeGeometry_.size() != nativeSceneModelData_.size())
                throw std::logic_error("MatDesc scene model table is out of sync with native geometry");
            materialModels.reserve(nativeGeometry_.size());
            for (const auto& geometry : nativeGeometry_) {
                const auto* instance = scene_.model(geometry.modelId);
                if (instance == nullptr)
                    throw std::logic_error("MatDesc scene model is no longer present in the scene");
                materialModels.push_back({.id = instance->id,
                                          .sourcePath = instance->sourcePath,
                                          .projectDirectory = currentProjectPath_.has_value()
                                                                  ? currentProjectPath_->parent_path()
                                                                  : std::filesystem::path{},
                                          .modelIndex = geometry.modelIndex,
                                          .vertexCount = geometry.baseVertices.size(),
                                          .cloneCount = geometry.cloneCount,
                                          .materials = instance->materialSettings});
            }
        }
        auto output = nativeRenderer_.recordFrame(commands, frameContext, nativeDirty, materials, lightSampling,
                                                  nativeRenderer_.environment(), executionResources, outputExecution,
                                                  materialModels);
        // Dirty flags describe pending scene changes, not a persistent state.
        // Consume them only after a native frame accepted the changes. A
        // failed frame must retain them for the next successful attempt.
        if (output.has_value())
            scene_.clearDirty();
        nativeOnStartPending_ = false;
        nativeFxPendingEvents_.clear();
        if (output.has_value() && outputExecution.sampleIndex + 1U == outputExecution.sampleCount)
            nativeScreenRuntime_.publishFrame(commands, output->texture);
        return output;
    } catch (const std::exception& exception) {
        // Keep the command buffer usable for the Preview fallback. Native
        // resources remain owned until the next renderer request, so a
        // failure cannot destroy objects referenced by commands already
        // recorded in this frame.
        log::warn("Native renderer frame failed; using Preview: ", exception.what());
        device_->setNativeRendererAvailability(false, false);
        device_->selectRenderer(graphics::RendererKind::preview);
        return std::nullopt;
    }
}

std::string Application::workspaceWindowName(const char* title, const char* id) const {
#if DAYO_HAS_IMGUI
    // Hash the original complete English name so existing imgui.ini docking layouts also survive the upgrade.
    return ui::label((std::string(title) + "##" + id + "." + workspaceSuffix(uiState_.workspace)).c_str());
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
#endif
    videoExportJob_.cancel();
    drainVideoReadbacks();
    videoExportUiActive_ = false;
    videoExportFramesFinished_ = false;
    videoExportRestorePending_ = false;
    activeVideoExport_.reset();
    videoRangeInitialized_ = false;
    scene_.clearProjectState();
    evaluatedModels_.models.clear();
    nativeControllerDeclarations_.clear();
    nativeRenderer_.reset();
    nativeDayoEnvironmentRuntime_.reset();
    nativeSceneFrame_.reset();
    nativeSceneResources_.reset();
    nativeSceneDerivedRuntime_.reset();
    nativeScreenRuntime_.reset();
    if (device_ != nullptr) {
        device_->setNativeRendererAvailability(false, false);
        device_->clearPreviewResources();
    }
    reloadedEffects_.clear();
    audioPlayer_.stop();
    audioSource_.clear();
    audioDestination_.fill('\0');
    audioFromSeconds_ = 0.0F;
    audioToSeconds_ = 0.0F;
    textures_.clear();
    animatedIndices_.clear();
    animatedMorphDeltas_.clear();
    animatedMorphRanges_.clear();
    animatedEffectiveVisibility_.clear();
    animatedVertexCount_ = 0;
    animatedMaterialTemplates_.clear();
    animatedTopologyGeneration_ = 0;
    nativeGeometry_.clear();
    nativeSceneModelRuntime_.reset();
    nativeSceneModelData_.clear();
    nativeLightSampling_.clear();
    nativeLightPowers_.clear();
    scheduledEffects_.clear();
    nativeDeformVersion_ = 0;
    mediaSeconds_ = 0.0;
    uploadedVideoFrame_ = -1;
    videoMode_ = false;
    videoVisible_ = false;
    mediaVideoError_.clear();
    backgroundVideoPath_.fill(0);
    animationFrame_ = 0.0F;
    uploadedAnimationFrame_ = -1;
    playing_ = true;
    repeat_ = true;
    audioVolume_ = 1.0F;
    audioOffsetSeconds_ = 0.0F;
    nativePlaybackWasActive_ = false;
    nativeOnStartPending_ = true;
    manualCamera_ = false;
#if DAYO_HAS_IMGUI
    cameraEditPreview_ = false;
    lightEditPreview_ = false;
    followVmdCamera_ = true;
    viewportInteraction_ = {};
    timelineEditor_ = {};
#endif
    cameraYaw_ = 0.0F;
    cameraPitch_ = 0.0F;
    cameraDistance_ = 3.0F;
    cameraFovRadians_ = 0.785398163F;
    cameraPerspective_ = true;
    cameraTarget_ = {};
    normalization_ = {};
    projectAssets_.clear();
    projectModelMetadata_.clear();
#if DAYO_HAS_IMGUI
    sequenceOutput_ = {};
    sequenceOutputDirectory_.fill(0);
    sequenceOutputFilename_.fill(0);
    const std::string_view defaultDirectory = "output";
    const std::string_view defaultFilename = "frame_00000.ppm";
    std::copy(defaultDirectory.begin(), defaultDirectory.end(), sequenceOutputDirectory_.begin());
    std::copy(defaultFilename.begin(), defaultFilename.end(), sequenceOutputFilename_.begin());
#endif
    projectEditorState_ = {};
    missingFocusModelProjectIndex_.reset();
    previewFocusState_ = {};
    previewFocusEvaluatedFrame_ = -1.0F;
    if (device_ != nullptr) {
        device_->cancelPreviewFocusPick();
        device_->setPreviewRenderSettings({});
    }
    previewHdriPath_.fill(0);
    failedPreviewHdriPath_.clear();
    previewHdriError_.clear();
    upstreamDocumentJson_.clear();
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
    auto device = graphics::createVulkanDevice(*window, options_.validation, options_.vulkan);
    device_ = device.get();
    nativeRenderer_.setEvaluationSnapshot(&evaluatedModels_);
    nativeOidnProvider_.setDevice(device_);
    std::unique_ptr<graphics::NativeEnvironmentBackend> environmentBackend;
    const graphics::EnvironmentPassBindings environmentBindings{
        .equirectToCubePipeline = device_->nativeEnvironmentEquirectPipeline(),
        .equirectToCubeLayout = device_->nativeEnvironmentEquirectLayout(),
        .prefilterPipeline = device_->nativeEnvironmentPrefilterPipeline(),
        .prefilterLayout = device_->nativeEnvironmentPrefilterLayout(),
    };
    if (environmentBindings.valid()) {
        environmentBackend = std::make_unique<graphics::NativeEnvironmentBackend>(*device_, environmentBindings);
        nativeRenderer_.setEnvironmentBackend(environmentBackend.get());
    }
    device_->setNativeFrameRecorder([this](graphics::CommandList& commands, const graphics::RenderTargetDesc& target) {
        return recordNativeFrame(commands, target);
    });
    const auto cleanupGraphicsRuntime = [this](void*) noexcept {
        nativeRenderer_.reset();
        nativeDayoEnvironmentRuntime_.reset();
        nativeSceneFrame_.reset();
        nativeSceneResources_.reset();
        nativeSceneDerivedRuntime_.reset();
        nativeScreenRuntime_.reset();
        nativeSceneModelRuntime_.reset();
        nativeRenderer_.setEnvironmentBackend(nullptr);
        device_ = nullptr;
    };
    std::unique_ptr<void, decltype(cleanupGraphicsRuntime)> runtimeCleanup(this, cleanupGraphicsRuntime);
    requestRenderer(options_.renderer);
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
    if (options_.videoExport) {
        nativeOnStartPending_ = true;
        return runVideoExport();
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
                // In GUI builds, mouse input is read only from the hovered ImGui viewport.
            case platform::WindowEvent::Type::cameraPanned:
            case platform::WindowEvent::Type::cameraZoomed:
                break;
#else
                ViewportNavigation::orbit(cameraYaw_, cameraPitch_, event.x, event.y);
                manualCamera_ = true;
                refreshPreviewScene();
                break;
            case platform::WindowEvent::Type::cameraPanned:
                ViewportNavigation::pan(cameraTarget_, event.x, event.y, normalization_.scale);
                manualCamera_ = true;
                refreshPreviewScene();
                break;
            case platform::WindowEvent::Type::cameraZoomed:
                ViewportNavigation::zoom(cameraDistance_, event.x, normalization_.scale);
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
                if (!videoReadbacks_.empty() && videoExportJob_.canAcceptFrame()) {
                    if (auto image = device_->collectRenderedImage(videoReadbacks_.front(), false)) {
                        videoExportJob_.submitFrame(std::move(*image));
                        videoReadbacks_.erase(videoReadbacks_.begin());
                    }
                }
                if (videoNextFrame_ < videoOutputFrameCount_ && videoExportJob_.canAcceptFrame() &&
                    videoReadbacks_.size() < 3) {
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
                            if (device_->supportsPipelinedReadback()) {
                                videoReadbacks_.push_back(
                                    device_->enqueueRenderToImage({exportOptions.width, exportOptions.height}));
                                ++videoNextFrame_;
                            } else if (videoExportJob_.trySubmitFrame(
                                           device_->renderToImage({exportOptions.width, exportOptions.height}))) {
                                ++videoNextFrame_;
                            }
                        } catch (const std::exception& exception) {
                            videoExportStatus_ = exception.what();
                            videoExportJob_.requestCancel();
                            videoExportFramesFinished_ = true;
                        }
                        videoPreviousSourceFrame_ = sourceFrame;
                    }
                } else if (videoNextFrame_ >= videoOutputFrameCount_ && videoReadbacks_.empty()) {
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
        } else if (scene_.advanceFrame(deltaSeconds * playbackSpeed_, playing_, repeat_)) {
            const bool wrapped =
                repeat_ && static_cast<double>(animationFrame_) +
                                   deltaSeconds * static_cast<double>(playbackSpeed_) * sceneTimelineFps(scene_) >=
                               scene_.timeline().duration + 1.0;

            animationFrame_ = scene_.timeline().frame;
            if (wrapped) {
                restartAudioAtCurrentFrame();
                previewFocusState_.trackingValid = false;
            }
            if (!repeat_ && animationFrame_ >= scene_.timeline().duration) {
                playing_ = false;
                audioPlayer_.setPaused(true);
            }
            // Pose, physics, camera and focus all use this rendered fractional
            // timeline sample. Do not evaluate a separate pose for autofocus.
            if (!scene_.models().empty()) {
                refreshAnimatedMesh(false, deltaSeconds * playbackSpeed_);
            }
        }
#if DAYO_HAS_IMGUI
        if (!imageSequenceExportRunning_)
#endif
            if (!videoExportUiActive_)
                refreshPreviewScene(deltaSeconds);
        auto* media = scene_.backgroundMedia();
        for (auto& effect : reloadedEffects_) {
            std::string reloadError;
            if (effect.reloader.poll(&reloadError) && effect.reloader.current() != nullptr) {
                static_cast<void>(scene_.removeEffect(effect.id));
                effect.id = scene_.addEffect(*effect.reloader.current(), effect.owner);
                requestRenderer(requestedRenderer_);
                log::info("Hot reloaded effect graph: ", effect.path.string());
            } else if (!reloadError.empty()) {
                log::warn("FX hot reload deferred: ", reloadError);
            }
        }
        // The shared time follows the timeline even while video is hidden.
        mediaSeconds_ = std::max(0.0, static_cast<double>(animationFrame_) / sceneTimelineFps(scene_));
        if (videoMode_ && videoVisible_ && media != nullptr && media->info().hasVideo) {
            const auto videoFrame =
                static_cast<std::int64_t>(backgroundVideoSeconds() * media->info().videoFramesPerSecond);
            if (videoFrame != uploadedVideoFrame_)
                refreshVideoFrame();
        }
        device->beginUiFrame();
        buildUi();
        if (playing_ && !nativePlaybackWasActive_)
            nativeOnStartPending_ = true;
        nativePlaybackWasActive_ = playing_;
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
    project.renderer = std::string(graphics::toString(requestedRenderer_));
    project.frame = animationFrame_;
    project.playing = playing_;
    project.upstreamDocumentJson = upstreamDocumentJson_;
    project.assets = projectAssets_;
    project.editor = projectEditorState_;
    if (device_ != nullptr)
        project.renderSettings = device_->previewRenderSettings();
    project.editor.animationRepeat = repeat_;
    project.editor.wavVolume = audioVolume_;
    project.editor.wavOffset = audioOffsetSeconds_;
    project.editor.floorCollision = scene_.physicsSettings().floorCollision;
    project.editor.animationSpeed = playbackSpeed_;
    project.editor.recordFps = static_cast<float>(sceneTimelineFps(scene_));
    project.editor.wavFile = audioSource_;
    project.editor.movieVisible = videoVisible_;
    project.editor.movieFile = videoMode_ && scene_.background().videoPath.has_value() ? *scene_.background().videoPath
                                                                                       : std::filesystem::path{};
#if DAYO_HAS_IMGUI
    auto sequenceFile = std::filesystem::path(sequenceOutputFilename_.data());
    sequenceFile.replace_extension(sequenceOutput_.format == core::OutputFormat::png   ? ".png"
                                   : sequenceOutput_.format == core::OutputFormat::exr ? ".exr"
                                                                                       : ".ppm");
    project.editor.outputFile = std::filesystem::path(sequenceOutputDirectory_.data()) / sequenceFile;
    project.editor.samplesPerFrame = sequenceOutput_.samples;
    project.editor.motionBlur = sequenceOutput_.motionBlur;
    project.editor.outputWidth = sequenceWidth_;
    project.editor.outputHeight = sequenceHeight_;
#endif
    project.models = projectModelMetadata_;
    for (const auto& instance : scene_.models()) {
        core::ProjectModelState state;
        state.instanceId = instance.instanceId;
        state.source = instance.sourcePath;
        auto savedModel = std::ranges::find_if(
            project.models, [&](const auto& previous) { return previous.instanceId == instance.instanceId; });
        if (savedModel != project.models.end())
            state.upstreamId = savedModel->upstreamId;
        if (instance.model != nullptr) {
            state.bones.reserve(instance.model->bones.size());
            for (const auto& bone : instance.model->bones)
                state.bones.push_back(bone.name);
            state.morphs.reserve(instance.model->morphs.size());
            for (const auto& morph : instance.model->morphs)
                state.morphs.push_back(morph.name);
            state.materials.reserve(instance.model->materials.size());
            for (const auto& material : instance.model->materials)
                state.materials.push_back(material.name);
        }
        state.materialAnnotations.reserve(instance.materialSettings.size());
        for (const auto& material : instance.materialSettings) {
            state.materialAnnotations.push_back(material.annotation.string());
            state.previewPbrPresets.push_back(material.previewPbrPreset);
        }
        state.motionOrder = instance.order.motion;
        state.deformOrder = instance.order.deform;
        state.postprocessOrder = instance.order.postprocess;
        state.rasterOrder = instance.order.raster;
        state.cloneCount = instance.cloneCount;
        state.visible = instance.visible;
        if (savedModel == project.models.end())
            project.models.push_back(std::move(state));
        else
            *savedModel = std::move(state);
    }
    auto& focusIndex = project.renderSettings.dof.modelIndex;
    if (focusIndex < scene_.models().size())
        focusIndex = previewFocusProjectIndex(scene_.models(), project.models, focusIndex);
    else if (missingFocusModelProjectIndex_)
        focusIndex = *missingFocusModelProjectIndex_;
    for (auto& asset : project.assets) {
        if (asset.kind != "effect")
            continue;
        const auto normalized = std::filesystem::absolute(asset.path).lexically_normal();
        const auto loaded =
            std::ranges::find_if(reloadedEffects_, [&](const auto& effect) { return effect.path == normalized; });
        if (loaded == reloadedEffects_.end())
            continue;
        const auto owner = loaded->owner.has_value()
                               ? std::ranges::find(scene_.models(), *loaded->owner, &core::ModelInstance::id)
                               : scene_.models().end();
        if (owner != scene_.models().end()) {
            const auto ownerState = std::ranges::find_if(
                project.models, [&](const auto& state) { return state.instanceId == owner->instanceId; });
            if (ownerState != project.models.end()) {
                asset.ownerModelIndex = static_cast<std::size_t>(std::distance(project.models.begin(), ownerState));
                asset.upstreamId =
                    ownerState->upstreamId.value_or(static_cast<std::int32_t>(*asset.ownerModelIndex + 1U));
            } else {
                asset.ownerModelIndex.reset();
            }
        } else {
            asset.ownerModelIndex.reset();
        }
        const auto* graph = loaded->reloader.current();
        if (graph == nullptr)
            continue;
        if (graph->category == "render")
            asset.upstreamId = -1;
        if (!loaded->materialSourceFiles.empty())
            asset.materialSourceFiles = loaded->materialSourceFiles;
        if (!graph->materialDescriptor.has_value())
            continue;
        if (!asset.materialSourceFiles.empty())
            continue;
        const auto defaultFile = graph->materialDescriptor->defaultFile.generic_string();
        asset.materialSourceFiles.clear();
        asset.materialSourceFiles.reserve(project.models.size());
        for (const auto& model : project.models) {
            std::vector<std::string> sources;
            sources.reserve(model.materials.size());
            for (std::size_t material = 0; material < model.materials.size(); ++material) {
                const auto annotation = material < model.materialAnnotations.size()
                                            ? std::filesystem::path(model.materialAnnotations[material])
                                            : std::filesystem::path{};
                sources.push_back(annotation.empty() ? defaultFile : annotation.generic_string());
            }
            asset.materialSourceFiles.push_back(std::move(sources));
        }
    }
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
            core::MediaFile media(source, core::MediaOpenMode::audioOnly);
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
                try {
                    core::MediaFile media(source, core::MediaOpenMode::audioOnly);
                    if (std::find(candidates.begin(), candidates.end(), source) == candidates.end())
                        candidates.push_back(source);
                } catch (const core::NoAudioStreamError&) {
                    // Silent videos are valid scene assets but cannot supply export audio.
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
        core::MediaFile media(*audioSource, core::MediaOpenMode::audioOnly);
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
        if (videoMode_ && scene_.backgroundMedia() != nullptr) {
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
    std::deque<std::uint64_t> readbacks;
    auto previousSourceFrame = static_cast<float>(firstFrame);
    for (std::uint64_t outputFrame = 0; outputFrame < frameCount; ++outputFrame) {
        const auto sourceFrame = videoSourceFrame(outputFrame, firstFrame, lastFrame, sourceFps, options.fps);
        if (outputFrame != 0U) {
            const auto deltaSeconds = std::max(0.0F, sourceFrame - previousSourceFrame) * sourceFrameDuration;
            evaluateFrame(sourceFrame, deltaSeconds, false);
        }
        previousSourceFrame = sourceFrame;
        if (device_->supportsPipelinedReadback()) {
            readbacks.push_back(device_->enqueueRenderToImage({request.width, request.height}));
            if (readbacks.size() == 3) {
                exporter.writeVideoFrame(device_->collectRenderedImage(readbacks.front()).value());
                readbacks.pop_front();
            }
        } else {
            exporter.writeVideoFrame(device_->renderToImage({request.width, request.height}));
        }
        if (outputFrame + 1U == frameCount || outputFrame == 0U ||
            (outputFrame + 1U) % std::max<std::uint64_t>(1U, frameCount / 20U) == 0U) {
            log::info("Video export: ", outputFrame + 1U, "/", frameCount, " frames");
        }
    }
    while (!readbacks.empty()) {
        exporter.writeVideoFrame(device_->collectRenderedImage(readbacks.front()).value());
        readbacks.pop_front();
    }
    device_->waitIdle();
    const auto result = exporter.finish();
    log::info("Exported MP4: ", result.output.string(), " (", result.durationSeconds, " s, ", result.encodedFrames,
              " frames)");
    return 0;
}

void Application::loadEffectAsset(const std::filesystem::path& path, std::optional<core::ModelId> owner) {
    const auto normalizedPath = std::filesystem::absolute(path).lexically_normal();
    if (owner.has_value()) {
        const auto unowned = std::ranges::find_if(
            reloadedEffects_, [&](const auto& loaded) { return loaded.path == normalizedPath && !loaded.owner; });
        if (unowned != reloadedEffects_.end()) {
            static_cast<void>(scene_.removeEffect(unowned->id));
            reloadedEffects_.erase(unowned);
        }
    }
    const auto duplicate = std::ranges::find_if(
        reloadedEffects_, [&](const auto& loaded) { return loaded.path == normalizedPath && loaded.owner == owner; });
    if (duplicate != reloadedEffects_.end())
        return;

    ReloadedEffect reloaded(normalizedPath, owner);
    static_cast<void>(reloaded.reloader.poll());
    if (reloaded.reloader.current() == nullptr)
        throw std::runtime_error("effect graph is empty");
    const auto graph = *reloaded.reloader.current();
    reloaded.id = scene_.addEffect(graph, owner);
    reloadedEffects_.push_back(std::move(reloaded));
    auto filename = normalizedPath.filename().string();
    std::ranges::transform(filename, filename.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (device_ != nullptr && filename.find("subayai") != std::string::npos)
        requestedRenderer_ = graphics::RendererKind::subayai;
    else if (device_ != nullptr && filename.find("bdpt") != std::string::npos)
        requestedRenderer_ = graphics::RendererKind::bdpt;
    requestRenderer(requestedRenderer_);
    lastAsset_ = ui::Message("Effect {0} — passes: {1}, textures: {2}",
                             {normalizedPath.filename().string(), std::to_string(graph.passes.size()),
                              std::to_string(graph.textures.size())});
    log::info("Loaded effect graph: ", lastAsset_);
    const auto projectEffect = std::ranges::find_if(projectAssets_, [&](const auto& asset) {
        return asset.kind == "effect" && std::filesystem::absolute(asset.path).lexically_normal() == normalizedPath;
    });
    if (projectEffect == projectAssets_.end())
        projectAssets_.emplace_back("effect", normalizedPath);
}

void Application::handleAsset(const std::filesystem::path& path) {
    const auto kind = core::classifyAsset(path);
    if (kind == core::AssetKind::unknown) {
        lastAsset_ = ui::Message("Unsupported asset: {0}", {path.string()});
        log::warn(lastAsset_);
        return;
    }
    if (kind == core::AssetKind::project) {
        try {
            auto project = core::loadProject(path);
            // Upgrade legacy references by source occurrence before any load can
            // fail. A missing duplicate must not consume the next instance's ID.
            std::vector<bool> assigned(project.models.size(), false);
            for (auto& state : project.models)
                if (state.instanceId.empty())
                    state.instanceId = core::newModelInstanceId();
            for (const auto& asset : project.assets)
                if (!asset.modelInstanceId.empty())
                    for (std::size_t i = 0; i < project.models.size(); ++i)
                        if (project.models[i].instanceId == asset.modelInstanceId)
                            assigned[i] = true;
            for (auto& asset : project.assets) {
                if (asset.kind != "pmx" || !asset.modelInstanceId.empty())
                    continue;
                for (std::size_t i = 0; i < project.models.size(); ++i) {
                    if (assigned[i] || !samePreviewFocusSource(asset.path, project.models[i].source))
                        continue;
                    asset.modelInstanceId = project.models[i].instanceId;
                    assigned[i] = true;
                    break;
                }
            }
            currentProjectPath_ = std::filesystem::absolute(path).lexically_normal();
#if DAYO_HAS_IMGUI
            const auto projectText = currentProjectPath_->string();
            const auto projectLength = std::min(projectText.size(), projectDestination_.size() - 1U);
            std::copy_n(projectText.data(), projectLength, projectDestination_.data());
            projectDestination_[projectLength] = '\0';
#endif
            resetProjectRuntimeState();
            upstreamDocumentJson_ = project.upstreamDocumentJson;
            projectModelMetadata_ = project.models;
            projectEditorState_ = project.editor;
            if (device_ != nullptr) {
                device_->cancelPreviewFocusPick();
                device_->setPreviewRenderSettings(project.renderSettings);
            }
            previewHdriPath_.fill(0);
            const auto lightingPath = projectEditorState_.skyboxFile.string();
            std::copy_n(lightingPath.data(), std::min(lightingPath.size(), previewHdriPath_.size() - 1U),
                        previewHdriPath_.data());
            repeat_ = project.editor.animationRepeat;
            audioVolume_ = project.editor.wavVolume;
            audioOffsetSeconds_ = static_cast<float>(project.editor.wavOffset);
            auto physicsSettings = scene_.physicsSettings();
            physicsSettings.floorCollision = project.editor.floorCollision;
            scene_.setPhysicsSettings(physicsSettings);
            if (project.renderer == "subayai")
                requestRenderer(graphics::RendererKind::subayai);
            else if (project.renderer == "bdpt")
                requestRenderer(graphics::RendererKind::bdpt);
            else
                requestRenderer(graphics::RendererKind::preview);
            playbackSpeed_ = project.editor.animationSpeed;
#if DAYO_HAS_IMGUI
            if (!project.editor.outputFile.empty()) {
                const auto directory = project.editor.outputFile.parent_path().string();
                const auto filename = project.editor.outputFile.filename().string();
                if (directory.size() >= sequenceOutputDirectory_.size() ||
                    filename.size() >= sequenceOutputFilename_.size())
                    throw std::length_error("project output filename exceeds the editor limit");
                sequenceOutputDirectory_.fill(0);
                sequenceOutputFilename_.fill(0);
                std::copy(directory.begin(), directory.end(), sequenceOutputDirectory_.begin());
                std::copy(filename.begin(), filename.end(), sequenceOutputFilename_.begin());
                auto extension = project.editor.outputFile.extension().string();
                std::ranges::transform(extension, extension.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                sequenceOutput_.format = extension == ".png"   ? core::OutputFormat::png
                                         : extension == ".exr" ? core::OutputFormat::exr
                                                               : core::OutputFormat::ppm;
            }
            sequenceOutput_.samples = project.editor.samplesPerFrame;
            sequenceOutput_.motionBlur = project.editor.motionBlur;
            sequenceOutput_.firstFrame = static_cast<std::uint32_t>(std::max(project.editor.recordStart, 0));
            if (project.editor.recordEnd >= 0)
                sequenceOutput_.lastFrame = static_cast<std::uint32_t>(project.editor.recordEnd);
            sequenceWidth_ = project.editor.outputWidth;
            sequenceHeight_ = project.editor.outputHeight;
#endif
            const auto findLoadedModel = [&](std::size_t projectIndex) -> core::ModelInstance* {
                const auto index =
                    previewFocusSceneIndex(scene_.models(), project.models, static_cast<std::uint32_t>(projectIndex));
                return index < scene_.models().size() ? scene_.model(scene_.models()[index].id) : nullptr;
            };
            for (const auto& asset : project.assets) {
                if (asset.kind != "pmx")
                    continue;
                try {
                    const auto previousCount = scene_.models().size();
                    handleAsset(asset.path);
                    if (scene_.models().size() > previousCount && !asset.modelInstanceId.empty()) {
                        auto* instance = scene_.selectedModel();
                        const auto loaded = std::ranges::find_if(projectAssets_, [&](const auto& entry) {
                            return entry.kind == "pmx" && entry.modelInstanceId == instance->instanceId;
                        });
                        if (loaded != projectAssets_.end())
                            loaded->modelInstanceId = asset.modelInstanceId;
                        instance->instanceId = asset.modelInstanceId;
                    }
                } catch (const std::exception& exception) {
                    log::warn("Project model could not be loaded: ", asset.path.string(), ": ", exception.what());
                }
            }
            if (device_ != nullptr && !project.models.empty()) {
                auto settings = device_->previewRenderSettings();
                const auto requestedIndex = settings.dof.modelIndex;
                settings.dof.modelIndex = previewFocusSceneIndex(scene_.models(), project.models, requestedIndex);
                if (settings.dof.modelIndex == previewMissingFocusModel)
                    missingFocusModelProjectIndex_ = requestedIndex;
                device_->setPreviewRenderSettings(settings);
            }
            for (std::size_t index = 0; index < project.models.size(); ++index) {
                auto* instance = findLoadedModel(index);
                if (instance == nullptr)
                    continue;
                const auto& state = project.models[index];
                instance->order = {state.motionOrder, state.deformOrder, state.postprocessOrder, state.rasterOrder};
                static_cast<void>(scene_.setCloneCount(instance->id, state.cloneCount));
                static_cast<void>(scene_.setModelVisible(instance->id, state.visible));
                const auto annotationCount =
                    std::min(instance->materialSettings.size(), state.materialAnnotations.size());
                for (std::size_t material = 0; material < annotationCount; ++material)
                    instance->materialSettings[material].annotation = state.materialAnnotations[material];
                const auto presetCount = std::min(instance->materialSettings.size(), state.previewPbrPresets.size());
                for (std::size_t material = 0; material < presetCount; ++material)
                    instance->materialSettings[material].previewPbrPreset = state.previewPbrPresets[material];
            }
            for (const auto& asset : project.assets) {
                if (asset.kind != "effect")
                    continue;
                try {
                    std::optional<core::ModelId> owner;
                    if (asset.ownerModelIndex.has_value()) {
                        if (auto* instance = findLoadedModel(*asset.ownerModelIndex); instance != nullptr) {
                            owner = instance->id;
                        } else {
                            log::warn("Project effect owner model is unavailable; skipping effect: ",
                                      asset.path.string());
                            continue;
                        }
                    }
                    loadEffectAsset(asset.path, owner);
                } catch (const std::exception& exception) {
                    log::warn("Project effect could not be loaded: ", asset.path.string(), ": ", exception.what());
                }
                const auto normalized = std::filesystem::absolute(asset.path).lexically_normal();
                const auto loaded = std::ranges::find_if(reloadedEffects_,
                                                         [&](const auto& effect) { return effect.path == normalized; });
                if (loaded != reloadedEffects_.end() && !asset.materialSourceFiles.empty())
                    loaded->materialSourceFiles = asset.materialSourceFiles;
            }
            for (const auto& asset : project.assets)
                if (asset.kind != "pmx" && asset.kind != "effect") {
                    try {
                        if (asset.kind == "video") {
                            const auto text = asset.path.string();
                            backgroundVideoPath_.fill(0);
                            std::copy_n(text.data(), std::min(text.size(), backgroundVideoPath_.size() - 1U),
                                        backgroundVideoPath_.data());
                            if (audioSource_.empty() && project.editor.wavFile.empty()) {
                                // Legacy projects stored movie audio only as a video asset.
                                try {
                                    core::MediaFile legacy(asset.path, core::MediaOpenMode::audioOnly);
                                    loadAudioSource(asset.path);
                                } catch (const core::NoAudioStreamError&) {
                                    // A silent legacy movie still remains a valid background.
                                }
                            }
                            loadBackgroundVideo(asset.path, project.editor.movieVisible);
                        } else {
                            handleAsset(asset.path);
                        }
                    } catch (const std::exception& exception) {
                        log::warn("Project asset could not be loaded: ", asset.path.string(), ": ", exception.what());
                    }
                }

            for (const auto& savedAsset : project.assets) {
                const auto existing = std::ranges::find_if(projectAssets_, [&](const auto& loaded) {
                    return loaded.kind == savedAsset.kind &&
                           (savedAsset.kind != "pmx" || loaded.modelInstanceId == savedAsset.modelInstanceId) &&
                           std::filesystem::absolute(loaded.path).lexically_normal() ==
                               std::filesystem::absolute(savedAsset.path).lexically_normal();
                });
                if (existing == projectAssets_.end()) {
                    projectAssets_.push_back(savedAsset);
                } else {
                    existing->ownerModelIndex = savedAsset.ownerModelIndex;
                    existing->upstreamId = savedAsset.upstreamId;
                    existing->materialSourceFiles = savedAsset.materialSourceFiles;
                }
            }
            if (project.embeddedMotions.size() > 1U) {
                scene_.attachMotion(project.embeddedMotions.front());
                const auto count = std::min(project.models.size(), project.embeddedMotions.size() - 1U);
                for (std::size_t index = 0; index < count; ++index) {
                    const auto& motion = project.embeddedMotions[index + 1U];
                    if (!motion.bones.empty() || !motion.morphs.empty() || !motion.ik.empty()) {
                        if (auto* instance = findLoadedModel(index); instance != nullptr)
                            scene_.attachMotion(motion, instance->id);
                    }
                }
                manualCamera_ = false;
#if DAYO_HAS_IMGUI
                followVmdCamera_ = true;
                cameraEditPreview_ = false;
#endif
            } else if (project.embeddedMotion) {
                scene_.attachMotion(*project.embeddedMotion, scene_.selectedModelId());
                manualCamera_ = false;
            }
            animationFrame_ = project.frame;
            scene_.setFrame(animationFrame_);
            playing_ = project.playing;
            syncMediaAtCurrentFrame();
            if (!scene_.models().empty())
                refreshAnimatedMesh(false);
            lastAsset_ = ui::Message("Project {0} — assets: {1}",
                                     {path.filename().string(), std::to_string(project.assets.size())});
            log::info("Loaded project: ", lastAsset_);
        } catch (const std::exception& exception) {
            lastAsset_ = ui::Message("Project error: {0}", {exception.what()});
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
            syncMediaAtCurrentFrame();
            refreshAnimatedMesh(false);
            refreshPreviewScene();
            lastAsset_ = ui::Message("VMdayo {0}", {path.filename().string()});
            projectAssets_.emplace_back("vmdayo", std::filesystem::absolute(path));
            log::info("Loaded VMdayo motion: ", path.string());
        } catch (const std::exception& exception) {
            lastAsset_ = ui::Message("VMdayo error: {0}", {exception.what()});
            log::warn(lastAsset_);
        }
        return;
    }
    if (kind == core::AssetKind::image) {
        try {
            auto image = core::loadImageRgba8(path);
            scene_.setBackgroundImage(path);
            videoVisible_ = false;
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
            lastAsset_ = ui::Message("Image {0} — {1}x{2}", {path.filename().string(), std::to_string(image.width),
                                                             std::to_string(image.height)});
            projectAssets_.emplace_back("image", std::filesystem::absolute(path));
            log::info("Loaded image: ", lastAsset_);
        } catch (const std::exception& exception) {
            lastAsset_ = ui::Message("Image error: {0}", {exception.what()});
            log::warn(lastAsset_);
        }
        return;
    }
    if (kind == core::AssetKind::pmx) {
        try {
            const auto modelId = scene_.addModel(path);
            scene_.selectModel(modelId);
            projectAssets_.emplace_back("pmx", std::filesystem::absolute(path));
            projectAssets_.back().modelInstanceId = scene_.selectedModel()->instanceId;
            if (scene_.models().empty() || scene_.selectedModelId() != modelId)
                throw std::logic_error("PMX model was not retained in the scene");
            log::info("PMX scene state: models=", scene_.models().size(), " selected=", scene_.selectedModelId());
            videoMode_ = scene_.backgroundMedia() != nullptr && scene_.backgroundMedia()->info().hasVideo;
            normalization_ = scene_.selectedModel()->normalization;
            if (const auto associatedEffect = core::findAssociatedEffect(path); associatedEffect.has_value()) {
                try {
                    loadEffectAsset(*associatedEffect, modelId);
                } catch (const std::exception& exception) {
                    log::warn("Associated FX could not be loaded for ", path.filename().string(), ": ",
                              exception.what());
                }
            }
            refreshPreviewTextures();
            animationFrame_ = 0.0F;
            scene_.setFrame(animationFrame_);
            syncMediaAtCurrentFrame();
            uploadedAnimationFrame_ = -1;
            refreshAnimatedMesh(true);
            if (videoMode_)
                refreshVideoFrame();
            refreshPreviewScene();
            const auto* model = selectedModel();
            lastAsset_ = ui::Message(
                "PMX {0} — v{1}, vertices: {2}, triangles: {3}, bones: {4}, models: {5}",
                {model->model->metadata.modelName, std::to_string(model->model->metadata.version),
                 std::to_string(model->model->metadata.vertexCount), std::to_string(model->model->indices.size() / 3),
                 std::to_string(model->model->bones.size()), std::to_string(scene_.models().size())});
            log::info("Loaded metadata: ", lastAsset_, " (", path.string(), ")");
        } catch (const std::exception& exception) {
            lastAsset_ = ui::Message("PMX error: {0}", {exception.what()});
            log::warn(lastAsset_);
        }
        return;
    }
    if (kind == core::AssetKind::audio || kind == core::AssetKind::video) {
        if (kind == core::AssetKind::video) {
            const auto text = std::filesystem::absolute(path).string();
            backgroundVideoPath_.fill(0);
            std::copy_n(text.data(), std::min(text.size(), backgroundVideoPath_.size() - 1U),
                        backgroundVideoPath_.data());
        }
        loadAudioSource(path);
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
            scene_.importMotion(std::move(motion));
            manualCamera_ = false;
#if DAYO_HAS_IMGUI
            followVmdCamera_ = true;
            cameraEditPreview_ = false;
#endif
            animationFrame_ = 0.0F;
            scene_.setFrame(animationFrame_);
            syncMediaAtCurrentFrame();
            if (selectedModel() != nullptr)
                refreshAnimatedMesh(false);
            refreshPreviewScene();
            if (cameraOnly) {
                lastAsset_ = ui::Message(
                    "VMD camera/light — camera keys: {0}, light keys: {1}, frames: {2}",
                    {std::to_string(cameraKeyCount), std::to_string(lightKeyCount), std::to_string(lastFrame)});
            } else {
                lastAsset_ = ui::Message("VMD {0} — bone keys: {1}, morph keys: {2}, frames: {3}",
                                         {motionName.empty() ? path.filename().string() : motionName,
                                          std::to_string(boneKeyCount), std::to_string(morphKeyCount),
                                          std::to_string(lastFrame)});
            }
            log::info("Loaded motion: ", lastAsset_, " (", path.string(), ")");
            projectAssets_.emplace_back("vmd", std::filesystem::absolute(path));
        } catch (const std::exception& exception) {
            lastAsset_ = ui::Message("VMD error: {0}", {exception.what()});
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
            lastAsset_ = ui::Message("VPD pose — bones: {0}", {std::to_string(pose->bones.size())});
            log::info("Loaded pose: ", lastAsset_, " (", path.string(), ")");
            projectAssets_.emplace_back("vpd", std::filesystem::absolute(path));
        } catch (const std::exception& exception) {
            lastAsset_ = ui::Message("VPD error: {0}", {exception.what()});
            log::warn(lastAsset_);
        }
        return;
    }
    if (kind == core::AssetKind::effect) {
        try {
            const auto owner =
                selectedModel() == nullptr ? std::nullopt : std::optional<core::ModelId>{selectedModel()->id};
            loadEffectAsset(path, owner);
        } catch (const std::exception& exception) {
            lastAsset_ = ui::Message("Effect error: {0}", {exception.what()});
            log::warn(lastAsset_);
        }
        return;
    }
    lastAsset_ = ui::Message("Asset {0}: {1}", {std::string(core::toString(kind)), path.filename().string()});
    log::info("Accepted ", core::toString(kind), " asset: ", path.string());
}

void Application::refreshAnimatedMesh(bool initialUpload, float deltaSeconds) {
    nativeFxPendingEvents_.latch(scene_.dirty(core::DirtyFlag::geometry), scene_.dirty(core::DirtyFlag::material));
    if (device_ == nullptr || scene_.models().empty())
        return;
    if (scene_.dirty(core::DirtyFlag::material)) {
        nativeMaterialGeneration_ = nativeMaterialGeneration_ == std::numeric_limits<std::uint64_t>::max()
                                        ? 1U
                                        : nativeMaterialGeneration_ + 1U;
    }
    frameScratch_.reset();
    auto* scratch = frameScratch_.resource();
    std::size_t vertexCount = 0;
    std::size_t indexCount = 0;
    std::size_t materialCount = 0;
    bool dynamicVertices = false;
    for (const auto& instance : scene_.models()) {
        if (instance.model == nullptr || instance.animator == nullptr)
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
        core::ModelExecutionPolicy policy;
        core::AnimatedModelFrame frame;
    };
    std::pmr::vector<EvaluatedModel> evaluated(scratch);
    evaluated.reserve(scene_.models().size());
    const auto executionPlan = core::ModelExecutionPlanner::plan(scene_.models(), scene_.effects());
    for (std::size_t index = 0; index < scene_.models().size(); ++index) {
        const auto& instance = scene_.models()[index];
        const auto& policy = executionPlan[index];
        if (!policy.evaluateAnimation)
            continue;
        const auto gravity = scene_.evaluatePhysicsSettings(animationFrame_);
        if (policy.evaluatePhysics && instance.physics != nullptr) {
            instance.physics->setGravity({gravity.gravityDirection[0] * gravity.gravity,
                                          gravity.gravityDirection[1] * gravity.gravity,
                                          gravity.gravityDirection[2] * gravity.gravity});
            instance.physics->setGravityNoise(gravity.noiseAmplitude, gravity.noiseFrequency);
            instance.physics->setFloorCollision(gravity.floorCollision);
        }
        evaluated.push_back({&instance, instance.softBody == nullptr || !instance.softBody->available(), policy, {}});
    }
    {
        auto animation = frameProfiler_.measure(core::ProfileSection::animation);
        std::vector<std::int32_t> motionOrderValues;
        motionOrderValues.reserve(evaluated.size());
        for (const auto& model : evaluated)
            motionOrderValues.push_back(model.instance->order.motion);
        const auto motionOrder = core::stableMotionEvaluationOrder(motionOrderValues);
        // Upstream evaluates models in motion order. Keep this sequential: a
        // later model may depend on state produced by an earlier model.
        for (const auto index : motionOrder) {
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
        }
        animation.finish();
    }
    for (auto& evaluatedModel : evaluated) {
        auto* instance = scene_.model(evaluatedModel.instance->id);
        if (instance == nullptr)
            continue;
        instance->animationVisible = evaluatedModel.frame.visible;
        evaluatedModel.policy = core::ModelExecutionPlanner::resolve(*instance, scene_.effects());
    }
    evaluatedModels_.models.clear();
    evaluatedModels_.models.reserve(evaluated.size());
    for (const auto& evaluatedModel : evaluated) {
        if (!evaluatedModel.policy.exposeToControllers)
            continue;
        const auto& instance = *evaluatedModel.instance;
        core::fx::EvaluatedModelState snapshot;
        snapshot.id = instance.id;
        snapshot.sourcePath = instance.sourcePath;
        snapshot.displayName = instance.displayName;
        snapshot.modelName = instance.model->metadata.modelName;
        snapshot.englishName = instance.model->metadata.englishName;
        snapshot.morphNames.reserve(instance.model->morphs.size());
        for (const auto& morph : instance.model->morphs)
            snapshot.morphNames.push_back(morph.name);
        snapshot.boneNames.reserve(instance.model->bones.size());
        for (const auto& bone : instance.model->bones)
            snapshot.boneNames.push_back(bone.name);
        snapshot.morphWeights = evaluatedModel.frame.morphWeights;
        snapshot.bones = evaluatedModel.frame.bones;
        evaluatedModels_.models.push_back(std::move(snapshot));
    }
    previewFocusBindings_.clear();
    std::size_t materialCursor = 0;
    std::uint32_t indexCursor = 0;
    std::pmr::vector<graphics::PreviewBoneTransform> bones(scratch);
    std::vector<NativeModelGeometry> nativeGeometry;
    nativeGeometry.reserve(evaluated.size());
    std::vector<graphics::NativeSceneModelData> nativeSceneModels;
    nativeSceneModels.reserve(evaluated.size());
    std::vector<std::uint8_t> effectiveVisibility;
    effectiveVisibility.reserve(evaluated.size());
    for (const auto& evaluatedModel : evaluated) {
        const auto& instance = *evaluatedModel.instance;
        const auto& frame = evaluatedModel.frame;
        const auto& policy = evaluatedModel.policy;
        effectiveVisibility.push_back(policy.rasterize ? 1U : 0U);
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
        previewFocusBindings_.push_back({instance.model, instance.normalization, instance.id,
                                         static_cast<std::uint32_t>(materialCursor),
                                         static_cast<std::uint32_t>(instance.model->materials.size()),
                                         static_cast<std::uint32_t>(boneBase), gpuSkinning});
        if (gpuSkinning) {
            bones.reserve(bones.size() + frame.bones.size());
            for (const auto& source : frame.bones)
                bones.push_back(makePreviewBone(source, instance.normalization));
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
        const auto sceneCloneCount = std::max(instance.cloneCount, 1U);
        const auto cloneCount =
            graphics::resolveNativeModelCloneCount(instance.cloneCount, instance.id, scene_.effects().deform);
        const auto baseVertex = static_cast<std::uint32_t>(vertices.size());
        const auto firstModelIndex = indexCursor;
        NativeModelGeometry native;
        native.meshId = static_cast<std::uint32_t>(nativeGeometry.size() + 1U);
        native.cloneCount = cloneCount;
        native.modelId = instance.id;
        native.modelIndex = static_cast<std::uint32_t>(nativeSceneModels.size());
        native.textureBase = static_cast<std::uint32_t>(textureBase);
        native.rasterizeOrder = static_cast<std::uint32_t>(std::max(instance.order.raster, 0));
        native.deformIndex = native.modelIndex;
        native.deformOrder = static_cast<std::uint32_t>(std::max(instance.order.deform, 0));
        native.rasterize = policy.rasterize;
        native.acceleration = policy.includeInTlas;
        native.hasBlas = policy.buildBlas;
        native.indices.assign(instance.model->indices.begin(), instance.model->indices.end());
        native.morphWeights.resize(instance.model->morphs.size(), 0.0F);
        for (std::size_t morphIndex = 0; morphIndex < native.morphWeights.size(); ++morphIndex) {
            if (morphIndex < frame.morphWeights.size())
                native.morphWeights[morphIndex] = frame.morphWeights[morphIndex];
        }
        std::vector<std::array<std::uint32_t, 2>> nativeMorphRanges(instance.model->vertices.size());
        std::vector<std::uint32_t> nativeMorphCounts(instance.model->vertices.size(), 0U);
        for (const auto& morph : instance.model->morphs) {
            if (morph.type != 1)
                continue;
            for (const auto& offset : morph.offsets) {
                if (offset.index < 0 || static_cast<std::size_t>(offset.index) >= nativeMorphCounts.size())
                    continue;
                ++nativeMorphCounts[static_cast<std::size_t>(offset.index)];
            }
        }
        std::size_t nativeMorphOffset = 0;
        for (std::size_t vertexIndex = 0; vertexIndex < nativeMorphRanges.size(); ++vertexIndex) {
            if (nativeMorphOffset > std::numeric_limits<std::uint32_t>::max())
                throw std::overflow_error("native morph delta count exceeds the native deform ABI");
            nativeMorphRanges[vertexIndex] = {static_cast<std::uint32_t>(nativeMorphOffset),
                                              nativeMorphCounts[vertexIndex]};
            nativeMorphOffset += nativeMorphCounts[vertexIndex];
        }
        native.morphDeltas.resize(nativeMorphOffset);
        std::vector<std::uint32_t> nativeMorphCursors;
        nativeMorphCursors.reserve(nativeMorphRanges.size());
        for (const auto range : nativeMorphRanges)
            nativeMorphCursors.push_back(range[0]);
        for (std::size_t morphIndex = 0; morphIndex < instance.model->morphs.size(); ++morphIndex) {
            const auto& morph = instance.model->morphs[morphIndex];
            if (morph.type != 1)
                continue;
            for (const auto& offset : morph.offsets) {
                if (offset.index < 0 || static_cast<std::size_t>(offset.index) >= nativeMorphCursors.size())
                    continue;
                graphics::PreviewMorphDelta delta;
                for (std::size_t axis = 0; axis < 3; ++axis)
                    delta.delta[axis] = offset.vector3[axis] * instance.normalization.scale;
                delta.morphIndex = static_cast<std::uint32_t>(morphIndex);
                native.morphDeltas[nativeMorphCursors[static_cast<std::size_t>(offset.index)]++] = delta;
            }
        }
        if (gpuSkinning) {
            native.bones.reserve(frame.bones.size());
            for (const auto& source : frame.bones)
                native.bones.push_back(makePreviewBone(source, instance.normalization));
        }
        native.baseVertices.reserve(frame.vertices.size());
        native.deformedVertices.reserve(frame.vertices.size());
        if (rebuildVertices) {
            auto conversion = frameProfiler_.measure(core::ProfileSection::vertexConvert);
            for (std::size_t sourceIndex = 0; sourceIndex < frame.vertices.size(); ++sourceIndex) {
                const auto& source = frame.vertices[sourceIndex];
                const auto morphRange =
                    rebuildTopology ? sourceMorphRanges[sourceIndex] : animatedMorphRanges_[baseVertex + sourceIndex];
                const auto vertex = makePreviewVertex(source, frame.bones.size(), boneBase, instance.normalization,
                                                      morphRange, gpuSkinning);
                vertices.push_back(vertex);
                if (rebuildTopology)
                    animatedMorphRanges_.push_back(morphRange);
            }
            conversion.finish();
        }
        for (std::size_t sourceIndex = 0; sourceIndex < frame.vertices.size(); ++sourceIndex) {
            const auto& source = frame.vertices[sourceIndex];
            const auto vertex = makePreviewVertex(source, frame.bones.size(), 0, instance.normalization,
                                                  nativeMorphRanges[sourceIndex], gpuSkinning);
            native.baseVertices.push_back(vertex);

            graphics::NativeDeformedVertex seed;
            std::copy(std::begin(vertex.position), std::end(vertex.position), seed.position);
            seed.position[3] = 1.0F;
            std::copy(std::begin(vertex.normal), std::end(vertex.normal), seed.normal);
            seed.normal[3] = 0.0F;
            std::copy(std::begin(vertex.uv), std::end(vertex.uv), seed.uv);
            native.deformedVertices.push_back(seed);
        }
        if (!native.baseVertices.empty() && !native.indices.empty()) {
            auto nativeModel = graphics::makeNativeSceneModelData(*instance.model, native.baseVertices, frame.materials,
                                                                  instance.normalization, frame.vertices);
            const auto hasLoadedPmxTexture = [&instance](std::int32_t index) {
                return index >= 0 && static_cast<std::size_t>(index) < instance.model->textures.size() &&
                       hasLoadedTexture(instance.textures, index);
            };
            for (std::size_t materialIndex = 0; materialIndex < instance.model->materials.size(); ++materialIndex) {
                const auto& source = instance.model->materials[materialIndex];
                auto& linked = nativeModel.materials[materialIndex];
                if (!hasLoadedPmxTexture(source.textureIndex))
                    linked.tex = -1;
                if (!hasLoadedPmxTexture(source.sphereTextureIndex))
                    linked.spTex = -1;
            }
            nativeModel.topologyGeneration = scene_.topologyGeneration();
            nativeModel.materialGeneration = nativeMaterialGeneration_;
            nativeSceneModels.push_back(std::move(nativeModel));
            nativeGeometry.push_back(std::move(native));
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
            material.roughness = std::clamp(std::sqrt(2.0F / (std::max(material.shininess, 0.0F) + 2.0F)), 0.18F, 0.9F);
            material.metallic = 0.0F;
            material.specularStrength = 0.5F;
            material.skin = 0.0F;
            material.anisotropy = 0.0F;
            material.sheen = material.clearcoat = 0.0F;
            const auto preset = materialIndex < instance.materialSettings.size()
                                    ? instance.materialSettings[materialIndex].previewPbrPreset
                                    : 0U;
            switch (preset) {
            case 1: // Skin
                material.roughness = 0.45F;
                material.specularStrength = 0.35F;
                material.skin = 1.0F;
                break;
            case 2: // Hair
                material.roughness = 0.30F;
                material.anisotropy = 0.65F;
                break;
            case 3: // Cloth
                material.sheen = 0.6F;
                material.roughness = 0.70F;
                break;
            case 4: // Metal
                material.roughness = 0.25F;
                material.metallic = 0.8F;
                break;
            case 5: // Plastic
                material.clearcoat = 0.5F;
                material.roughness = 0.38F;
                break;
            case 6: // Glass
                material.clearcoat = 1.0F;
                material.roughness = 0.08F;
                material.specularStrength = 1.0F;
                break;
            default:
                break;
            }
            auto& draw = draws[materialCursor - 1U];
            draw.firstIndex = firstIndex;
            draw.indexCount = policy.rasterize ? sourceMaterial.indexCount : 0U;
            draw.materialIndex = static_cast<std::uint32_t>(materialCursor - 1U);
            draw.instanceCount = sceneCloneCount;
            firstIndex += instance.model->materials[materialIndex].indexCount;
        }
    }
    const bool participationChanged = effectiveVisibility != animatedEffectiveVisibility_;
    animatedEffectiveVisibility_ = std::move(effectiveVisibility);
    nativeGeometry_ = std::move(nativeGeometry);
    nativeSceneModelData_ = std::move(nativeSceneModels);
    nativeDeformVersion_ =
        nativeDeformVersion_ == std::numeric_limits<std::uint64_t>::max() ? 1U : nativeDeformVersion_ + 1U;
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
        if (initialUpload || rebuildTopology || participationChanged)
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
    // Preview synchronizes these immediately, but native accumulation must
    // still observe topology/material changes in recordNativeFrame().
    if (device_->activeRenderer() == graphics::RendererKind::preview)
        scene_.clearDirty(core::DirtyFlag::geometry | core::DirtyFlag::material);
}

void Application::refreshPreviewTextures() {
    if (device_ == nullptr)
        return;
    textures_.clear();
    for (const auto& instance : scene_.models()) {
        textures_.insert(textures_.end(), instance.textures.begin(), instance.textures.end());
    }
    nativeSceneDerivedRuntime_.reset();
    std::vector<graphics::PreviewTexture> previewTextures;
    previewTextures.reserve(textures_.size());
    for (const auto& texture : textures_) {
        previewTextures.push_back({texture.width, texture.height, texture.pixels, hasTransparentPixels(texture)});
    }
    device_->uploadPreviewTextures(previewTextures);
}

void Application::restartAudioAtCurrentFrame() {
    if (loadedAudio_.samples.empty())
        return;
    const double seconds = std::max(0.0, static_cast<double>(animationFrame_) / sceneTimelineFps(scene_) +
                                             static_cast<double>(audioOffsetSeconds_));
    audioPlayer_.play(loadedAudio_, seconds);
    audioPlayer_.setVolume(audioVolume_);
    audioPlayer_.setPlaybackSpeed(playbackSpeed_);
    audioPlayer_.setPaused(!playing_);
}

void Application::seekTimeline(float frame) {
    animationFrame_ = std::clamp(frame, 0.0F, scene_.timeline().duration);
    scene_.setFrame(animationFrame_);
    syncMediaAtCurrentFrame();
    refreshAnimatedMesh(false);
    refreshPreviewScene();
}

void Application::syncMediaAtCurrentFrame() {
    mediaSeconds_ = std::max(0.0, static_cast<double>(animationFrame_) / sceneTimelineFps(scene_));
    uploadedVideoFrame_ = -1;
    if (videoMode_)
        refreshVideoFrame();
    restartAudioAtCurrentFrame();
}

void Application::loadAudioSource(const std::filesystem::path& path) {
    try {
        core::MediaFile source(path, core::MediaOpenMode::audioOnly);
        auto audio = options_.videoExport ? core::AudioBuffer{} : source.decodeAudio();
        scene_.setMedia(path, core::MediaPresentation::audioOnly);
        animationFrame_ = scene_.timeline().frame;
        audioPlayer_.stop();
        loadedAudio_ = std::move(audio);
        waveformPeaks_.clear();
        if (!options_.videoExport) {
            waveformPeaks_.assign(1024, 0.0F);
            if (!loadedAudio_.samples.empty() && loadedAudio_.channels != 0) {
                const auto frames = loadedAudio_.samples.size() / loadedAudio_.channels;
                for (std::size_t bucket = 0; bucket < waveformPeaks_.size(); ++bucket) {
                    const auto begin = bucket * frames / waveformPeaks_.size();
                    const auto end = std::max((bucket + 1) * frames / waveformPeaks_.size(), begin + 1);
                    float peak = 0.0F;
                    for (auto frame = begin; frame < std::min(end, frames); ++frame) {
                        for (std::uint32_t channel = 0; channel < loadedAudio_.channels; ++channel) {
                            peak =
                                std::max(peak, std::abs(loadedAudio_.samples[frame * loadedAudio_.channels + channel]));
                        }
                    }
                    waveformPeaks_[bucket] = peak;
                }
            }
        }
        audioSource_ = std::filesystem::absolute(path).lexically_normal();
        setAudioExportDestinationForSource(path);
        audioToSeconds_ = static_cast<float>(std::max(0.0, source.info().durationSeconds));
        std::erase_if(projectAssets_, [](const auto& asset) { return asset.kind == "audio"; });
        projectAssets_.emplace_back("audio", audioSource_);
        restartAudioAtCurrentFrame();
        lastAsset_ =
            ui::Message("Audio {0} — {1} s", {path.filename().string(), std::to_string(source.info().durationSeconds)});
        log::info("Loaded audio: ", path.string());
    } catch (const std::exception& error) {
        lastAsset_ = ui::Message("Audio error: {0}", {error.what()});
        log::warn(lastAsset_);
    }
}

void Application::loadBackgroundVideo(const std::filesystem::path& path, bool visible) {
    try {
        mediaVideoError_.clear();
        scene_.setMedia(path, core::MediaPresentation::backgroundVideo);
        animationFrame_ = scene_.timeline().frame;
        videoMode_ = true;
        setVideoVisible(visible);
        std::erase_if(projectAssets_, [](const auto& asset) { return asset.kind == "video"; });
        projectAssets_.emplace_back("video", std::filesystem::absolute(path).lexically_normal());
        lastAsset_ = ui::Message("Background video {0}", {path.filename().string()});
        log::info("Loaded background video: ", path.string());
    } catch (const std::exception& error) {
        mediaVideoError_ = error.what();
        lastAsset_ = ui::Message("Background video error: {0}", {mediaVideoError_});
        log::warn(lastAsset_);
    }
}

void Application::setVideoVisible(bool visible) {
    const auto* media = scene_.backgroundMedia();
    videoMode_ = media != nullptr && media->info().hasVideo;
    videoVisible_ = visible && videoMode_;
    scene_.setBackgroundVideoVisible(videoVisible_);
    if (videoVisible_)
        scene_.setBackgroundEnabled(true);
    mediaSeconds_ = std::max(0.0, static_cast<double>(animationFrame_) / sceneTimelineFps(scene_));
    uploadedVideoFrame_ = -1;
    refreshPreviewBackground();
    refreshPreviewScene();
}

void Application::selectBackgroundSource(core::ScreenTextureSource source) {
    if (source == core::ScreenTextureSource::backgroundVideo) {
        std::error_code pathError;
        const auto requested = std::filesystem::absolute(backgroundVideoPath_.data(), pathError).lexically_normal();
        if (!pathError && videoMode_ && scene_.background().videoPath == requested)
            setVideoVisible(true);
        else
            loadBackgroundVideo(backgroundVideoPath_.data());
        return;
    }
    setVideoVisible(false);
    scene_.setBackgroundScreenSource(source);
    refreshPreviewBackground();
    refreshPreviewScene();
}

void Application::buildMediaPlaybackControls(bool compact) {
#if DAYO_HAS_IMGUI
    if (!audioSource_.empty())
        ImGui::TextWrapped(ui::tr("Audio: %s"), audioSource_.filename().string().c_str());
    else
        ImGui::TextDisabled("%s", ui::tr("Drop an MP4 or audio file to load audio."));
    const auto videoPath = videoMode_ && scene_.background().videoPath.has_value()
                               ? *scene_.background().videoPath
                               : std::filesystem::path(backgroundVideoPath_.data());
    if (!videoPath.empty()) {
        if (compact && !audioSource_.empty())
            ImGui::SameLine();
        ImGui::TextWrapped(ui::tr("Video: %s"), videoPath.filename().string().c_str());
    }
    const bool canShowVideo = videoMode_ || backgroundVideoPath_[0] != '\0';
    ImGui::BeginDisabled(!canShowVideo);
    bool show = videoVisible_;
    if (ImGui::Checkbox(ui::label("Show MP4 video"), &show)) {
        if (show)
            selectBackgroundSource(core::ScreenTextureSource::backgroundVideo);
        else
            setVideoVisible(false);
    }
    ImGui::EndDisabled();
    if (!canShowVideo)
        ImGui::TextDisabled("%s", ui::tr("Drop an MP4 to enable video display."));
    if (!mediaVideoError_.empty())
        ImGui::TextWrapped(ui::tr("Video: %s"), mediaVideoError_.c_str());
    ImGui::BeginDisabled(loadedAudio_.samples.empty());
    if (compact) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0F);
    }
    if (ImGui::SliderFloat(ui::label("Volume"), &audioVolume_, 0.0F, 1.0F))
        audioPlayer_.setVolume(audioVolume_);
    if (compact) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0F);
    }
    if (ImGui::DragFloat(ui::label("Audio offset"), &audioOffsetSeconds_, 0.01F, -60.0F, 60.0F, "%.2f s"))
        restartAudioAtCurrentFrame();
    ImGui::EndDisabled();
#else
    (void)compact;
#endif
}

void Application::buildMediaBackgroundUi() {
#if DAYO_HAS_IMGUI
    ImGui::InputTextWithHint(ui::label("Video file"), ui::tr("MP4 background (optional)"), backgroundVideoPath_.data(),
                             backgroundVideoPath_.size());
    if (ImGui::Button(ui::label("Apply video file")))
        loadBackgroundVideo(backgroundVideoPath_.data());
    buildMediaPlaybackControls();
#endif
}

void Application::refreshPreviewBackground() {
    if (device_ == nullptr)
        return;
    const auto& background = scene_.background();
    uploadedVideoFrame_ = -1;
    if (background.screenSource == core::ScreenTextureSource::backgroundVideo) {
        if (videoMode_ && videoVisible_ && scene_.backgroundMedia() != nullptr &&
            scene_.backgroundMedia()->info().hasVideo)
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

double Application::backgroundVideoSeconds() const {
    const auto* media = scene_.backgroundMedia();
    if (media == nullptr || media->info().durationSeconds <= 0.0)
        return mediaSeconds_;
    const auto& info = media->info();
    if (repeat_)
        return std::fmod(mediaSeconds_, info.durationSeconds);
    const double fps = info.videoFramesPerSecond > 0.0 ? info.videoFramesPerSecond : 30.0;
    return std::min(mediaSeconds_, std::max(0.0, info.durationSeconds - 1.0 / fps));
}

void Application::refreshVideoFrame() {
    auto* media = scene_.backgroundMedia();
    if (!videoMode_ || !videoVisible_ || media == nullptr || device_ == nullptr ||
        scene_.background().screenSource != core::ScreenTextureSource::backgroundVideo)
        return;
    const auto frameIndex = static_cast<std::int64_t>(backgroundVideoSeconds() * media->info().videoFramesPerSecond);
    if (frameIndex == uploadedVideoFrame_)
        return;
    const auto image = media->decodeVideoFrame(backgroundVideoSeconds());
    const std::array textures{graphics::PreviewTexture{image.width, image.height, image.pixels}};
    device_->uploadPreviewBackground(textures);
    if (scene_.models().empty()) {
        device_->updatePreviewMaterials({});
        device_->updatePreviewDraws({});
    }
    uploadedVideoFrame_ = frameIndex;
}

void Application::refreshPreviewScene(float focusDeltaOverride) {
    if (device_ == nullptr)
        return;
    const auto* model = selectedModel();
    const auto* globalLightMotion = scene_.cameraMotion();
    const auto* motion = globalLightMotion != nullptr && !globalLightMotion->lights.empty()
                             ? globalLightMotion
                             : (model != nullptr ? model->motion.get() : nullptr);
    graphics::PreviewScene scene;
    scene.animationTimeSeconds = animationFrame_ / static_cast<float>(sceneTimelineFps(scene_));
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
    const auto cameraState = makeSceneCameraState();
    std::copy(cameraState.rotation.begin(), cameraState.rotation.end(), scene.cameraRotation);
    scene.cameraDistance = cameraState.distance;
    std::copy(cameraState.position.begin(), cameraState.position.end(), scene.target);
    scene.verticalFovRadians = cameraState.verticalFovRadians;
    scene.perspective = cameraState.perspective;
    const graphics::PreviewFocusCamera focusCamera{cameraState.rotation, cameraState.position, cameraState.distance,
                                                   cameraState.verticalFovRadians, cameraState.perspective};
    const auto dof = device_->previewRenderSettings().dof;
    const float targetUnits = resolvePreviewFocusDistance(dof, focusCamera, scene_.models(), evaluatedModels_);
    const float targetMetres = graphics::previewDofLens(dof, targetUnits, cameraState.verticalFovRadians, 1U,
                                                        graphics::PreviewDofQuality::balanced)
                                   .focusMetres;
    const bool followsModel = dof.focusMode == graphics::PreviewFocusMode::bone ||
                              dof.focusMode == graphics::PreviewFocusMode::face ||
                              dof.focusMode == graphics::PreviewFocusMode::eyes ||
                              dof.focusMode == graphics::PreviewFocusMode::trackedSurface;
    const bool valid =
        !followsModel || (dof.modelIndex < scene_.models().size() &&
                          previewFocusBonePoint(scene_.models()[dof.modelIndex], evaluatedModels_, dof).has_value());
    const float focusDelta = focusDeltaOverride >= 0.0F           ? focusDeltaOverride
                             : previewFocusEvaluatedFrame_ < 0.0F ? 0.0F
                                                                  : (animationFrame_ - previewFocusEvaluatedFrame_) /
                                                                        static_cast<float>(sceneTimelineFps(scene_));
    if (!previewFocusState_.trackingValid || focusDelta != 0.0F || previewFocusState_.targetMetres != targetMetres)
        graphics::updatePreviewFocusState(previewFocusState_, targetMetres, focusDelta, dof, valid, !followsModel);
    previewFocusEvaluatedFrame_ = animationFrame_;
    scene.focusDistance = (previewFocusState_.currentMetres - dof.focusOffsetMetres) / dof.metresPerUnit;
    scene.debugMaterial = previewDebugMaterial_;
    scene.debugFlags = previewDebugFlags_;
    scene.outlineEnabled = previewOutlineEnabled_;
    if (motion != nullptr && !motion->lights.empty()) {
        const auto light = core::evaluateLight(*motion, animationFrame_);
        std::copy(light.position.begin(), light.position.end(), scene.lightDirection);
        std::copy(light.color.begin(), light.color.end(), scene.lightColor);
    }
#if DAYO_HAS_IMGUI
    if (lightEditPreview_) {
        std::copy(editedLight_.position.begin(), editedLight_.position.end(), scene.lightDirection);
        std::copy(editedLight_.color.begin(), editedLight_.color.end(), scene.lightColor);
    }
#endif
    device_->updatePreviewScene(scene);
}

void Application::resetPhysicsSimulation() {
    previewFocusState_ = {};
    previewFocusEvaluatedFrame_ = -1.0F;
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
    if (videoMode_ && scene_.backgroundMedia() != nullptr) {
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
        const auto* globalCameraMotion = scene_.cameraMotion();
        const auto* sourceMotion = globalCameraMotion != nullptr && !globalCameraMotion->cameras.empty()
                                       ? globalCameraMotion
                                       : (model ? model->motion.get() : nullptr);
        const bool hasCameraVmd = sourceMotion != nullptr && !sourceMotion->cameras.empty();
        const auto normalization = model != nullptr ? model->normalization : normalization_;
        const auto openInspector = [&](InspectorPage page) {
            inspectorPage_ = page;
            if (!uiState_.inspectorVisible) {
                uiState_.inspectorVisible = true;
                uiState_.resetLayoutRequested = true;
            }
            inspectorFocusRequested_ = true;
        };
        const auto resetView = [&] {
            ViewportNavigation::reset(cameraTarget_, cameraYaw_, cameraPitch_, cameraDistance_, normalization.center,
                                      normalization.scale);
            cameraFovRadians_ = 0.785398163F;
            cameraPerspective_ = true;
            cameraEditPreview_ = false;
            manualCamera_ = true;
            followVmdCamera_ = false;
            refreshPreviewScene();
        };
        if (ImGui::Button(ui::label("Camera")))
            openInspector(InspectorPage::camera);
        ImGui::SameLine();
        if (ImGui::Button(ui::label("Light")))
            openInspector(InspectorPage::light);
        ImGui::SameLine();
        if (ImGui::Button(ui::label("Effects")))
            openInspector(InspectorPage::effects);
        ImGui::SameLine();
        if (ImGui::Button(ui::label("Reset view")))
            resetView();
        if (hasCameraVmd) {
            bool follow = followVmdCamera_;
            if (ImGui::Checkbox(ui::label("Follow VMD camera"), &follow)) {
                if (follow) {
                    manualCamera_ = false;
                    cameraEditPreview_ = false;
                } else {
                    const auto current = makeSceneCameraState();
                    cameraTarget_ = current.position;
                    cameraYaw_ = current.rotation[1];
                    cameraPitch_ = current.rotation[0];
                    cameraDistance_ = current.distance;
                    cameraFovRadians_ = current.verticalFovRadians;
                    cameraPerspective_ = current.perspective;
                    manualCamera_ = true;
                    cameraEditPreview_ = false;
                }
                followVmdCamera_ = follow;
                refreshPreviewScene();
            }
        }
        if (hasCameraVmd && followVmdCamera_)
            ImGui::TextWrapped("%s", ui::tr("VMD camera locked. Turn off Follow VMD camera to navigate."));
        else
            ImGui::TextWrapped("%s", ui::tr("Left-drag: orbit | Right-drag: pan | Wheel: zoom"));
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
                const auto input = viewportInteraction_.image(ImTextureRef{static_cast<ImTextureID>(preview.textureId)},
                                                              available, !hasCameraVmd || !followVmdCamera_);
                uiState_.viewportHovered = input.hovered;
                if (input.focusPick) {
                    pendingTrackingFocus_ = input.focusTracking;
                    pendingFocusBindings_ = previewFocusBindings_;
                    pendingFocusSnapshot_ = evaluatedModels_;
                    trackingFocusFallback_ = false;
                    device_->requestPreviewFocusPick({input.focusUv.x, input.focusUv.y});
                }
                if (const auto point = device_->pollPreviewFocusSample()) {
                    auto settings = device_->previewRenderSettings();
                    settings.dof.focusMode = graphics::PreviewFocusMode::viewportPoint;
                    settings.dof.worldPoint = point->worldPoint;
                    if (pendingTrackingFocus_) {
                        const auto target = previewTrackedFocus(*point, pendingFocusBindings_, pendingFocusSnapshot_,
                                                                settings.dof.metresPerUnit);
                        const auto found =
                            target ? std::ranges::find(scene_.models(), target->model, &core::ModelInstance::id)
                                   : scene_.models().end();
                        if (target && found != scene_.models().end()) {
                            settings.dof.focusMode = graphics::PreviewFocusMode::trackedSurface;
                            settings.dof.modelIndex =
                                static_cast<std::uint32_t>(std::distance(scene_.models().begin(), found));
                            settings.dof.boneName = target->boneName;
                            settings.dof.localOffsetMetres = target->localOffsetMetres;
                            settings.dof.trackingInstance = target->instance;
                        } else {
                            trackingFocusFallback_ = true;
                        }
                    }
                    previewFocusState_.trackingValid = false;
                    settings.depthOfField.override = graphics::EffectOverride::enabled;
                    device_->setPreviewRenderSettings(settings);
                    refreshPreviewScene();
                }
                if (scene_.models().empty() && !videoVisible_) {
                    const auto* message = ui::tr("No model loaded\nDrop a PMX file into the window");
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
                if (input.orbit || input.pan || input.wheel != 0.0F) {
                    if (!manualCamera_) {
                        const auto current = makeSceneCameraState();
                        cameraTarget_ = current.position;
                        cameraYaw_ = current.rotation[1];
                        cameraPitch_ = current.rotation[0];
                        cameraDistance_ = current.distance;
                        cameraFovRadians_ = current.verticalFovRadians;
                        cameraPerspective_ = current.perspective;
                    }
                    cameraEditPreview_ = false;
                    const auto& io = ImGui::GetIO();
                    if (input.orbit)
                        ViewportNavigation::orbit(cameraYaw_, cameraPitch_, io.MouseDelta.x, io.MouseDelta.y);
                    if (input.pan)
                        ViewportNavigation::pan(cameraTarget_, io.MouseDelta.x, io.MouseDelta.y, normalization.scale);
                    if (input.wheel != 0.0F)
                        ViewportNavigation::zoom(cameraDistance_, input.wheel, normalization.scale);
                    manualCamera_ = true;
                    refreshPreviewScene();
                }
                if (input.contextMenu)
                    ImGui::OpenPopup("Viewport view controls");
                if (ImGui::BeginPopup("Viewport view controls")) {
                    if (ImGui::MenuItem(ui::label("Reset view")))
                        resetView();
                    if (ImGui::MenuItem(ui::label("Edit camera")))
                        openInspector(InspectorPage::camera);
                    if (ImGui::MenuItem(ui::label("Edit light")))
                        openInspector(InspectorPage::light);
                    ImGui::EndPopup();
                }
            }
        } else {
            device_->setPreviewViewportExtent({});
        }
    } else {
        device_->setPreviewViewportExtent({});
    }
    ImGui::End();

    if (uiState_.performanceVisible && ImGui::Begin(workspaceWindowName("Performance", "performance").c_str())) {
        const auto& totals = frameProfiler_.totals();
        ImGui::TextUnformatted(frameProfiler_.report().c_str());
        ImGui::Text(ui::tr("Upload: %.2f MiB/frame"),
                    totals.frames == 0
                        ? 0.0
                        : static_cast<double>(totals.uploadBytes) / static_cast<double>(totals.frames) / 1048576.0);
        ImGui::Text(ui::tr("Geometry: %llu vertices / %llu draws per frame"),
                    static_cast<unsigned long long>(totals.frames == 0 ? 0 : totals.vertices / totals.frames),
                    static_cast<unsigned long long>(totals.frames == 0 ? 0 : totals.draws / totals.frames));
        if (ImGui::Button(ui::label("Reset profiler")))
            frameProfiler_.reset();
    }
    if (uiState_.performanceVisible)
        ImGui::End();

    if (uiState_.sceneVisible && ImGui::Begin(workspaceWindowName("Scene", "scene").c_str())) {
        if (ImGui::Selectable(ui::label("Scene / Background"), uiState_.inspectScene)) {
            uiState_.inspectScene = true;
            inspectorPage_ = InspectorPage::selection;
        }
        ImGui::InputTextWithHint("##scene-filter", ui::tr("Search scene..."), uiState_.sceneFilter.data(),
                                 uiState_.sceneFilter.size());
        ImGui::SeparatorText(ui::tr("Models"));
        for (const auto& instance : scene_.models()) {
            if (uiState_.sceneFilter[0] != '\0' &&
                instance.displayName.find(uiState_.sceneFilter.data()) == std::string::npos)
                continue;
            ImGui::PushID(static_cast<int>(instance.id));
            bool selected = !uiState_.inspectScene && scene_.selectedModelId() == instance.id;
            const auto label = std::string(ui::tr(instance.visible ? "[visible]  " : "[hidden]  ")) +
                               instance.displayName + "###model-" + std::to_string(instance.id);
            if (ImGui::Selectable(label.c_str(), selected)) {
                uiState_.inspectScene = false;
                inspectorPage_ = InspectorPage::selection;
                scene_.selectModel(instance.id);
                normalization_ = instance.normalization;
                refreshAnimatedMesh(true);
                refreshPreviewScene();
            }
            if (ImGui::BeginPopupContextItem("scene-item-context")) {
                ImGui::MenuItem(ui::label("Rename"), nullptr, false, false);
                ImGui::MenuItem(ui::label("Duplicate / Clone"), nullptr, false, false);
                if (ImGui::MenuItem(ui::label("Open Source Folder"))) {
                    const auto url = "file://" + instance.sourcePath.parent_path().generic_string();
                    if (!SDL_OpenURL(url.c_str()))
                        lastAsset_ = ui::Message("Open folder: {0}", {SDL_GetError()});
                }
                ImGui::MenuItem(ui::label("Set External Parent"), nullptr, false, false);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    if (uiState_.sceneVisible)
        ImGui::End();

    if (uiState_.physicsVisible && ImGui::Begin(workspaceWindowName("Physics", "physics").c_str())) {
        ImGui::Text(ui::tr("Timeline: %.1f / %.1f frames"), animationFrame_, scene_.timeline().duration);
        ImGui::Checkbox(ui::label("Repeat"), &repeat_);
        if (ImGui::SliderFloat(ui::label("Playback speed"), &playbackSpeed_, 0.1F, 4.0F, "%.2fx"))
            restartAudioAtCurrentFrame();
        buildMediaPlaybackControls();
        if (!waveformPeaks_.empty()) {
            ImGui::PlotLines(ui::label("Waveform"), waveformPeaks_.data(), static_cast<int>(waveformPeaks_.size()), 0,
                             nullptr, 0.0F, 1.0F, {0.0F, 72.0F});
        }
        auto settings = scene_.physicsSettings();
        bool physicsChanged = ImGui::DragFloat(ui::label("Gravity"), &settings.gravity, 0.01F, 0.0F, 100.0F);
        physicsChanged |= ImGui::DragFloat3(ui::label("Gravity direction"), settings.gravityDirection.data(), 0.01F);
        physicsChanged |=
            ImGui::DragFloat(ui::label("Gravity noise amplitude"), &settings.noiseAmplitude, 0.01F, 0.0F, 100.0F);
        physicsChanged |=
            ImGui::DragFloat(ui::label("Gravity noise frequency"), &settings.noiseFrequency, 0.01F, 0.0F, 100.0F);
        physicsChanged |= ImGui::Checkbox(ui::label("Floor collision"), &settings.floorCollision);
        if (physicsChanged)
            scene_.setPhysicsSettings(settings);
        int runtimeMode = static_cast<int>(scene_.runtimeMode());
        if (ImGui::Combo(ui::label("Runtime mode"), &runtimeMode, ui::items("Accumulate|Realtime|Idle"))) {
            history_.execute(scene_, std::make_unique<core::SetRuntimeModeCommand>(
                                         scene_.runtimeMode(), static_cast<core::RuntimeMode>(runtimeMode)));
        }
        ImGui::Text(ui::tr("Accumulated samples: %llu"), static_cast<unsigned long long>(scene_.accumulatedSamples()));
        if (ImGui::Button(ui::label("Reset physics")) && model != nullptr && model->physics != nullptr)
            model->physics->reset();
        ImGui::SameLine();
        if (ImGui::Button(ui::label("Update one frame")) && model != nullptr) {
            refreshAnimatedMesh(false, 1.0F / 30.0F);
            refreshPreviewScene();
        }
        ImGui::Checkbox(ui::label("Rigid body debug"), &physicsDebug_);
        if (physicsDebug_ && model != nullptr && model->physics != nullptr) {
            const auto count = model->physics->bodyCount();
            if (ImGui::BeginChild("rigid-body-debug", {0.0F, 120.0F}, true)) {
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(count));
                while (clipper.Step())
                    for (int body = clipper.DisplayStart; body < clipper.DisplayEnd; ++body) {
                        const auto transform = model->physics->bodyTransform(static_cast<std::size_t>(body));
                        ImGui::Text(ui::tr("%d  mode %u  P %.2f %.2f %.2f"), body,
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
            const auto camera = makeSceneCameraState();
            const auto* model = selectedModel();
            const auto normalization = model != nullptr ? model->normalization : normalization_;
            const auto scale = ViewportNavigation::safeScale(normalization.scale);
            for (std::size_t i = 0; i < 3; ++i)
                key.position[i] = camera.position[i] / scale + normalization.center[i];
            key.distance = -camera.distance / scale;
            key.rotation = camera.rotation;
            key.viewAngle = camera.verticalFovRadians * 57.2957795131F;
            key.perspective = camera.perspective;
            std::erase_if(document.cameras, [&](const auto& item) { return item.frame == key.frame; });
            document.cameras.push_back(key);
            core::MotionEditor::normalize(document);
            history_.execute(scene_, std::make_unique<core::EditMotionCommand>(
                                         0, true, before, core::toVmdMotion(std::move(document), before.modelName),
                                         "Record camera key"));
        } else {
            playing_ = !playing_;
            restartAudioAtCurrentFrame();
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
    if (workspace == ui::Workspace::camera)
        inspectorPage_ = InspectorPage::camera;
    if (workspace == ui::Workspace::render)
        inspectorPage_ = InspectorPage::effects;
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
    ui::localizeImGui();
    if (!ImGui::BeginMainMenuBar())
        return;
    if (ImGui::BeginMenu(ui::label("File"))) {
        if (ImGui::MenuItem(ui::label("Save"), "Ctrl+S"))
            saveProjectNow();
        if (ImGui::MenuItem(ui::label("Save As...")))
            uiState_.saveAsOpen = true;
        ImGui::Separator();
        if (ImGui::MenuItem(ui::label("Quit"), "Alt+F4")) {
            SDL_Event event{};
            event.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&event);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(ui::label("Edit"))) {
        const bool canUndo = history_.canUndo();
        const bool canRedo = history_.canRedo();
        if (ImGui::MenuItem(ui::label("Undo"), "Ctrl+Z", false, canUndo) && history_.undo(scene_)) {
            animationFrame_ = scene_.timeline().frame;
            refreshAnimatedMesh(false);
            refreshPreviewScene();
        }
        if (ImGui::MenuItem(ui::label("Redo"), "Ctrl+Y", false, canRedo) && history_.redo(scene_)) {
            animationFrame_ = scene_.timeline().frame;
            refreshAnimatedMesh(false);
            refreshPreviewScene();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(ui::label("View"))) {
        if (ImGui::MenuItem(ui::label("Scene"), nullptr, &uiState_.sceneVisible))
            uiState_.resetLayoutRequested = true;
        if (ImGui::MenuItem(ui::label("Inspector"), nullptr, &uiState_.inspectorVisible))
            uiState_.resetLayoutRequested = true;
        if (ImGui::MenuItem(ui::label("Timeline"), nullptr, &uiState_.timelineVisible))
            uiState_.resetLayoutRequested = true;
        ImGui::MenuItem(ui::label("Status bar"), nullptr, &uiState_.statusBarVisible);
        ImGui::Separator();
        if (ImGui::MenuItem(ui::label("Reset Layout"))) {
            uiState_.resetLayoutRequested = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(ui::label("Media"))) {
        buildMediaPlaybackControls();
        if (ImGui::MenuItem(ui::label("Open Timeline"))) {
            uiState_.timelineVisible = true;
            uiState_.resetLayoutRequested = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(ui::label("Animation"))) {
        if (ImGui::MenuItem(playing_ ? ui::label("Pause") : ui::label("Play"), "Space")) {
            playing_ = !playing_;
            restartAudioAtCurrentFrame();
        }
        ImGui::MenuItem(ui::label("Loop"), nullptr, &repeat_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(ui::label("Render"))) {
        if (ImGui::MenuItem(ui::label("Export Video...")))
            uiState_.videoExportOpen = true;
        if (ImGui::MenuItem(ui::label("Export Audio...")))
            uiState_.audioExportOpen = true;
        if (ImGui::MenuItem(ui::label("Export Image Sequence..."))) {
            uiState_.imageSequenceExportOpen = true;
            setWorkspace(ui::Workspace::render);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(ui::label("Help"))) {
        ImGui::TextUnformatted(ui::tr("Space  Play / Pause"));
        ImGui::TextUnformatted(ui::tr("Ctrl+Z / Ctrl+Y  Undo / Redo"));
        ImGui::TextUnformatted(ui::tr("Left-drag: Orbit / Right-drag: Pan / Wheel: Zoom / Right-click: Reset view"));
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(ui::label("Language"))) {
        for (const auto candidate : ui::languages) {
            if (ImGui::MenuItem(ui::languageName(candidate), nullptr, ui::language() == candidate)) {
                ui::setLanguage(candidate);
                languageSaveFailed_ = !ui::persistEditorLanguage();
            }
        }
        if (languageSaveFailed_)
            ImGui::TextUnformatted(ui::tr("Could not save language preference."));
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
    if (!ImGui::BeginViewportSideBar("##workspace-bar", ImGui::GetMainViewport(), ImGuiDir_Up,
                                     ImGui::GetFrameHeightWithSpacing(),
                                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted(ui::tr("Workspace"));
    ImGui::PushID("WorkspaceSelector");
    const auto workspaceButton = [&](const char* source, ui::Workspace workspace) {
        const auto* label = ui::label(source);
        ImGui::SameLine();
        if (ImGui::Selectable(
                label, uiState_.workspace == workspace, ImGuiSelectableFlags_DontClosePopups,
                {ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0F, 0}))
            setWorkspace(workspace);
    };
    workspaceButton("Layout", ui::Workspace::layout);
    workspaceButton("Animation", ui::Workspace::animation);
    workspaceButton("Camera", ui::Workspace::camera);
    workspaceButton("Render", ui::Workspace::render);
    workspaceButton("Debug", ui::Workspace::debug);
    ImGui::PopID();
    ImGui::End();
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
    const float inspectorRatio = uiState_.workspace == ui::Workspace::camera ? 0.30F : 0.28F;
    const float timelineRatio = uiState_.workspace == ui::Workspace::animation ? 0.43F : 0.36F;
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
    ImGui::DockBuilderFinish(dockspaceId);
    uiState_.resetLayoutRequested = false;
#endif
}

void Application::captureCameraForEditing() {
#if DAYO_HAS_IMGUI
    const auto camera = makeSceneCameraState();
    const auto* model = selectedModel();
    const auto normalization = model != nullptr ? model->normalization : normalization_;
    const auto scale = ViewportNavigation::safeScale(normalization.scale);
    for (std::size_t axis = 0; axis < 3; ++axis)
        editedCamera_.position[axis] = camera.position[axis] / scale + normalization.center[axis];
    editedCamera_.rotation = camera.rotation;
    editedCamera_.distance = -camera.distance / scale;
    editedCamera_.viewAngle = camera.verticalFovRadians * 57.2957795131F;
    editedCamera_.perspective = camera.perspective;
#endif
}

void Application::captureLightForEditing() {
#if DAYO_HAS_IMGUI
    const auto* globalMotion = scene_.cameraMotion();
    const auto* model = selectedModel();
    const auto* motion = globalMotion != nullptr && !globalMotion->lights.empty()
                             ? globalMotion
                             : (model != nullptr ? model->motion.get() : nullptr);
    editedLight_ = motion != nullptr && !motion->lights.empty()
                       ? core::evaluateLight(*motion, animationFrame_)
                       : core::VmdLightKey{0, {0.6F, 0.6F, 0.6F}, {-0.5F, -1.0F, 0.5F}};
#endif
}

void Application::buildCameraLightControls() {
#if DAYO_HAS_IMGUI
    const auto execute = [&](core::VmdMotion before, core::MotionDocument document, std::string label) {
        const auto modelName = before.modelName;
        history_.execute(scene_, std::make_unique<core::EditMotionCommand>(
                                     0, true, std::move(before), core::toVmdMotion(std::move(document), modelName),
                                     std::move(label)));
        timelineEditor_.clearSelection();
        refreshAnimatedMesh(false);
        refreshPreviewScene();
    };
    const auto frame = static_cast<std::uint32_t>(std::max(animationFrame_, 0.0F));
    ImGui::Text(ui::tr("Frame %u  |  Camera / Lighting"), frame);
    ImGui::TextWrapped(
        "%s",
        ui::tr("Changes preview immediately. Register a key to save them at this frame; Ctrl+Z undoes registration."));
    ImGui::TextWrapped("%s", ui::tr("Register a key before exporting a VMD for reuse."));
    ImGui::PushItemWidth(-ImGui::GetFontSize() * 9.0F);

    if (inspectorPage_ == InspectorPage::camera) {
        if (!cameraEditPreview_)
            captureCameraForEditing();
        ImGui::SeparatorText(ui::tr("Camera"));
        if (ImGui::Button(ui::label("Use current view"))) {
            captureCameraForEditing();
            manualCamera_ = false;
            followVmdCamera_ = false;
            cameraEditPreview_ = true;
            refreshPreviewScene();
        }
        if (ImGui::Button(ui::label("Revert to animation##camera"))) {
            cameraEditPreview_ = false;
            manualCamera_ = false;
            followVmdCamera_ = true;
            captureCameraForEditing();
            refreshPreviewScene();
        }
        ImGui::Checkbox(ui::label("Realtime camera recording (Space registers)"), &recordCamera_);
        bool changed = false;
        changed |= ImGui::DragFloat3(ui::label("Target"), editedCamera_.position.data(), 0.01F);
        changed |= ImGui::DragFloat3(ui::label("Rotation (radians)"), editedCamera_.rotation.data(), 0.01F);
        changed |= ImGui::DragFloat(ui::label("Distance"), &editedCamera_.distance, 0.1F);
        if (editedCamera_.viewAngle <= 0.0F)
            editedCamera_.viewAngle = 30.0F;
        changed |= ImGui::SliderFloat(ui::label("Field of view"), &editedCamera_.viewAngle, 1.0F, 179.0F, "%.1f deg");
        changed |= ImGui::Checkbox(ui::label("Perspective"), &editedCamera_.perspective);
        if (ImGui::CollapsingHeader(ui::label("Camera parent (advanced)"))) {
            ImGui::InputInt(ui::label("Parent model"), &editedCamera_.parentModel);
            ImGui::InputInt(ui::label("Parent bone"), &editedCamera_.parentBone);
            ImGui::InputText(ui::label("Parent bone name"), cameraParentBoneName_.data(), cameraParentBoneName_.size());
        }
        if (changed) {
            manualCamera_ = false;
            followVmdCamera_ = false;
            cameraEditPreview_ = true;
            refreshPreviewScene();
        }
        if (ImGui::Button(ui::label("Register camera key"))) {
            core::VmdMotion before = scene_.cameraMotion() ? *scene_.cameraMotion() : core::VmdMotion{};
            auto document = core::toMotionDocument(before);
            editedCamera_.frame = frame;
            editedCamera_.parentBoneName = cameraParentBoneName_.data();
            std::erase_if(document.cameras, [&](const auto& key) { return key.frame == frame; });
            document.cameras.push_back(editedCamera_);
            core::MotionEditor::normalize(document);
            cameraEditPreview_ = false;
            manualCamera_ = false;
            followVmdCamera_ = true;
            execute(std::move(before), std::move(document), "Register camera key");
        }
    }
    if (inspectorPage_ == InspectorPage::light) {
        if (!lightEditPreview_)
            captureLightForEditing();
        ImGui::SeparatorText(ui::tr("Directional light"));
        if (ImGui::Button(ui::label("Revert to animation##light"))) {
            lightEditPreview_ = false;
            captureLightForEditing();
            scene_.markDirty(core::DirtyFlag::lighting);
            refreshPreviewScene();
        }
        bool changed = false;
        changed |= ImGui::ColorEdit3(ui::label("Color"), editedLight_.color.data());
        changed |= ImGui::DragFloat3(ui::label("Direction"), editedLight_.position.data(), 0.01F, -1.0F, 1.0F);
        if (changed) {
            lightEditPreview_ = true;
            scene_.markDirty(core::DirtyFlag::lighting);
            refreshPreviewScene();
        }
        if (ImGui::Button(ui::label("Register light key"))) {
            core::VmdMotion before = scene_.cameraMotion() ? *scene_.cameraMotion() : core::VmdMotion{};
            auto document = core::toMotionDocument(before);
            editedLight_.frame = frame;
            std::erase_if(document.lights, [&](const auto& key) { return key.frame == frame; });
            document.lights.push_back(editedLight_);
            core::MotionEditor::normalize(document);
            lightEditPreview_ = false;
            scene_.markDirty(core::DirtyFlag::lighting);
            execute(std::move(before), std::move(document), "Register light key");
        }
    }
    if (inspectorPage_ == InspectorPage::light && ImGui::CollapsingHeader(ui::label("Self shadow"))) {
        int shadowMode = editedShadow_.mode;
        if (ImGui::Combo(ui::label("Mode##self-shadow-mode"), &shadowMode, ui::items("None|Mode 1|Mode 2")))
            editedShadow_.mode = static_cast<std::uint8_t>(shadowMode);
        ImGui::DragFloat(ui::label("Shadow distance"), &editedShadow_.distance, 0.1F, 0.0F, 10'000.0F);
        if (ImGui::Button(ui::label("Register self shadow"))) {
            core::VmdMotion before = scene_.cameraMotion() ? *scene_.cameraMotion() : core::VmdMotion{};
            auto document = core::toMotionDocument(before);
            editedShadow_.frame = frame;
            std::erase_if(document.shadows, [&](const auto& key) { return key.frame == frame; });
            document.shadows.push_back(editedShadow_);
            core::MotionEditor::normalize(document);
            execute(std::move(before), std::move(document), "Register self shadow key");
        }
    }
    ImGui::SeparatorText(ui::tr("Reusable camera / light motion"));
    ImGui::InputText(ui::label("VMD file##camera-light"), cameraLightVmdPath_.data(), cameraLightVmdPath_.size());
    if (inspectorPage_ == InspectorPage::camera && ImGui::Button(ui::label("Export camera VMD"))) {
        try {
            if (cameraLightVmdPath_[0] == '\0')
                throw std::invalid_argument("Choose a VMD destination");
            if (scene_.cameraMotion() == nullptr || scene_.cameraMotion()->cameras.empty())
                throw std::invalid_argument("No global camera keys to export");
            core::VmdMotion extracted;
            extracted.modelName = "Camera/Light";
            extracted.cameras = scene_.cameraMotion()->cameras;
            extracted.lastFrame = extracted.cameras.back().frame;
            core::saveVmd(cameraLightVmdPath_.data(), extracted);
            cameraLightVmdStatus_ = "Camera exported";
        } catch (const std::exception& error) {
            cameraLightVmdStatus_ = error.what();
        }
    }
    if (inspectorPage_ == InspectorPage::light && ImGui::Button(ui::label("Export light VMD"))) {
        try {
            if (cameraLightVmdPath_[0] == '\0')
                throw std::invalid_argument("Choose a VMD destination");
            if (scene_.cameraMotion() == nullptr || scene_.cameraMotion()->lights.empty())
                throw std::invalid_argument("No global light keys to export");
            core::VmdMotion extracted;
            extracted.modelName = "Camera/Light";
            extracted.lights = scene_.cameraMotion()->lights;
            extracted.lastFrame = extracted.lights.back().frame;
            core::saveVmd(cameraLightVmdPath_.data(), extracted);
            cameraLightVmdStatus_ = "Light exported";
        } catch (const std::exception& error) {
            cameraLightVmdStatus_ = error.what();
        }
    }
    if (inspectorPage_ == InspectorPage::camera && ImGui::Button(ui::label("Import camera keys"))) {
        try {
            if (cameraLightVmdPath_[0] == '\0')
                throw std::invalid_argument("Choose a VMD source");
            const auto imported = core::loadVmd(cameraLightVmdPath_.data());
            if (imported.cameras.empty())
                throw std::invalid_argument("VMD has no camera keys");
            core::VmdMotion before = scene_.cameraMotion() ? *scene_.cameraMotion() : core::VmdMotion{};
            auto document = core::toMotionDocument(before);
            document.cameras = imported.cameras;
            core::MotionEditor::normalize(document);
            manualCamera_ = false;
            cameraEditPreview_ = false;
            followVmdCamera_ = true;
            execute(std::move(before), std::move(document), "Import camera keys");
            cameraLightVmdStatus_ = "Camera imported (other tracks preserved)";
        } catch (const std::exception& error) {
            cameraLightVmdStatus_ = error.what();
        }
    }
    if (inspectorPage_ == InspectorPage::light && ImGui::Button(ui::label("Import light keys"))) {
        try {
            if (cameraLightVmdPath_[0] == '\0')
                throw std::invalid_argument("Choose a VMD source");
            const auto imported = core::loadVmd(cameraLightVmdPath_.data());
            if (imported.lights.empty())
                throw std::invalid_argument("VMD has no light keys");
            core::VmdMotion before = scene_.cameraMotion() ? *scene_.cameraMotion() : core::VmdMotion{};
            auto document = core::toMotionDocument(before);
            document.lights = imported.lights;
            core::MotionEditor::normalize(document);
            lightEditPreview_ = false;
            scene_.markDirty(core::DirtyFlag::lighting);
            execute(std::move(before), std::move(document), "Import light keys");
            cameraLightVmdStatus_ = "Light imported (other tracks preserved)";
        } catch (const std::exception& error) {
            cameraLightVmdStatus_ = error.what();
        }
    }
    if (!cameraLightVmdStatus_.empty())
        ImGui::TextWrapped("%s", ui::tr(cameraLightVmdStatus_.c_str()));
    ImGui::PopItemWidth();
#endif
}

void Application::buildRenderSettingsControls() {
#if DAYO_HAS_IMGUI
    using namespace graphics;
    if (device_ == nullptr)
        return;
    ImGui::SeparatorText(ui::tr("Render Settings"));
    if (device_->activeRenderer() != RendererKind::preview) {
        ImGui::TextWrapped("%s", ui::tr("These effects apply to the Preview renderer and its exports."));
    }
    auto settings = device_->previewRenderSettings();
    const auto resolved = device_->previewResolvedEffects();
    bool changed = false;
    const auto combo = [&](const char* label, auto& value, const char* choices) {
        int selected = static_cast<int>(value);
        if (ImGui::Combo(label, &selected, choices)) {
            value = static_cast<std::remove_reference_t<decltype(value)>>(selected);
            changed = true;
        }
    };
    combo(ui::label("Quality preset"), settings.preset, ui::items("Auto|Performance|Balanced|High"));
    if (ImGui::Button(ui::label("Reset effect overrides"))) {
        for (auto* setting :
             {&settings.ambientOcclusion, &settings.softShadows, &settings.skinSubsurface, &settings.hairShading,
              &settings.clothSheen, &settings.clearcoat, &settings.bloom, &settings.depthOfField})
            *setting = {};
        settings.shadowFilter = PreviewShadowFilter::automatic;
        settings.antiAliasing = PreviewAntiAliasing::automatic;
        changed = true;
    }
    ImGui::TextWrapped("%s", ui::tr("Checked effects are active. Reset restores the selected preset."));
    const auto effect = [&](const char* name, PreviewEffectSetting& setting, bool active, std::uint32_t bit) {
        ImGui::PushID(name);
        const bool supported = (device_->previewEffectSupport() & bit) != 0;
        ImGui::BeginDisabled(!supported);
        if (ImGui::Checkbox(name, &active)) {
            setting.override = active ? EffectOverride::enabled : EffectOverride::disabled;
            changed = true;
        }
        if (setting.override != EffectOverride::inherit) {
            ImGui::SameLine();
            if (ImGui::SmallButton(ui::label("Reset"))) {
                setting = {};
                changed = true;
            }
        }
        ImGui::EndDisabled();
        if (!supported)
            ImGui::TextWrapped("%s", ui::tr("Unavailable on this device."));
        if (supported && ImGui::TreeNode(ui::label("Details"))) {
            ImGui::BeginDisabled(!active);
            changed |= ImGui::SliderFloat(ui::label("Intensity"), &setting.intensity, 0.0F, 4.0F, "%.2f");
            ImGui::EndDisabled();
            ImGui::TreePop();
        }
        ImGui::PopID();
    };
    if (ImGui::CollapsingHeader(ui::label("Lighting"), ImGuiTreeNodeFlags_DefaultOpen)) {
        effect(ui::label("Ambient occlusion"), settings.ambientOcclusion, resolved.ambientOcclusion,
               previewEffectSupportAmbientOcclusion);
        combo(ui::label("AO method"), settings.ambientOcclusionMode, ui::items("SSAO (legacy)|GTAO"));
        effect(ui::label("Soft shadows"), settings.softShadows, resolved.softShadows, previewEffectSupportSoftShadows);
        combo(ui::label("Shadow filter"), settings.shadowFilter, ui::items("PCF|PCSS|Auto"));
    }
    if (ImGui::CollapsingHeader(ui::label("Materials"), ImGuiTreeNodeFlags_DefaultOpen)) {
        effect(ui::label("Skin scattering"), settings.skinSubsurface, resolved.skinSubsurface,
               previewEffectSupportSkinSubsurface);
        effect(ui::label("Hair highlights"), settings.hairShading, resolved.hairShading,
               previewEffectSupportHairShading);
        effect(ui::label("Cloth sheen"), settings.clothSheen, resolved.clothSheen, previewEffectSupportClothSheen);
        effect(ui::label("Clearcoat"), settings.clearcoat, resolved.clearcoat, previewEffectSupportClearcoat);
        ImGui::TextWrapped(
            "%s", ui::tr("Choose Skin, Hair, Cloth, Plastic or Glass in the material inspector to use these effects."));
    }
    if (ImGui::CollapsingHeader(ui::label("Post processing"), ImGuiTreeNodeFlags_DefaultOpen)) {
        effect(ui::label("Bloom"), settings.bloom, resolved.bloom, previewEffectSupportBloom);
        ImGui::BeginDisabled(!resolved.bloom);
        changed |= ImGui::SliderFloat(ui::label("Bloom threshold"), &settings.bloomThreshold, 0.0F, 5.0F, "%.2f");
        ImGui::EndDisabled();
        effect(ui::label("Depth of field"), settings.depthOfField, resolved.depthOfField,
               previewEffectSupportDepthOfField);
        if (resolved.depthOfField && ImGui::TreeNodeEx(ui::label("Lens and focus"), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushItemWidth(-1.0F);
            const auto lensSlider = [&](const char* name, float& value, float low, float high, const char* format,
                                        ImGuiSliderFlags flags = 0) {
                ImGui::TextWrapped("%s", ui::tr(name));
                ImGui::PushID(name);
                changed |= ImGui::SliderFloat("##value", &value, low, high, format, flags);
                ImGui::PopID();
            };
            const auto lensCombo = [&](const char* name, auto& value, const char* choices) {
                ImGui::TextWrapped("%s", ui::tr(name));
                ImGui::PushID(name);
                combo("##value", value, choices);
                ImGui::PopID();
            };
            lensSlider("Aperture (F-number)", settings.dof.fNumber, 0.7F, 32.0F, "f/%.1f",
                       ImGuiSliderFlags_Logarithmic);
            lensCombo("Focus target", settings.dof.focusMode,
                      ui::items("Camera target|Manual distance|Bone|Face|Eyes|Static point|Tracking point"));
            lensCombo("DoF quality", settings.dof.quality, ui::items("Auto|Performance|Balanced|High"));
            if (settings.dof.focusMode == PreviewFocusMode::manual) {
                lensSlider("Focus distance (m)", settings.dof.focusDistanceMetres, 0.05F, 100.0F, "%.3f",
                           ImGuiSliderFlags_Logarithmic);
            }
            if (settings.dof.focusMode == PreviewFocusMode::bone || settings.dof.focusMode == PreviewFocusMode::face ||
                settings.dof.focusMode == PreviewFocusMode::eyes ||
                settings.dof.focusMode == PreviewFocusMode::trackedSurface) {
                const auto& models = scene_.models();
                const auto index = settings.dof.modelIndex;
                const char* selectedName =
                    index < models.size() ? models[index].displayName.c_str() : ui::tr("Missing focus model");
                ImGui::TextWrapped("%s", ui::tr("Focus model"));
                if (ImGui::BeginCombo("##focus-model", selectedName)) {
                    for (std::size_t i = 0; i < models.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::Selectable(models[i].displayName.c_str(), i == index)) {
                            settings.dof.modelIndex = static_cast<std::uint32_t>(i);
                            changed = true;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                if ((settings.dof.focusMode == PreviewFocusMode::bone ||
                     settings.dof.focusMode == PreviewFocusMode::trackedSurface) &&
                    settings.dof.modelIndex < models.size() && models[settings.dof.modelIndex].model != nullptr) {
                    const auto& bones = models[settings.dof.modelIndex].model->bones;
                    ImGui::TextWrapped("%s", ui::tr("Focus bone"));
                    if (ImGui::BeginCombo("##focus-bone", settings.dof.boneName.c_str())) {
                        for (const auto& bone : bones) {
                            if (ImGui::Selectable(bone.name.c_str(), settings.dof.boneName == bone.name)) {
                                settings.dof.boneName = bone.name;
                                changed = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                }
                const auto* selected = selectedModel();
                if (selected != nullptr && selected->model != nullptr && !selected->model->bones.empty() &&
                    ImGui::Button(ui::label("Focus selected bone"))) {
                    const auto found = std::ranges::find(models, selected->id, &core::ModelInstance::id);
                    settings.dof.modelIndex = static_cast<std::uint32_t>(std::distance(models.begin(), found));
                    const auto boneIndex =
                        std::clamp(selectedBone_, 0, static_cast<int>(selected->model->bones.size() - 1U));
                    settings.dof.boneName = selected->model->bones[static_cast<std::size_t>(boneIndex)].name;
                    settings.dof.focusMode = PreviewFocusMode::bone;
                    changed = true;
                }
                if (settings.dof.modelIndex >= models.size() ||
                    !previewFocusBonePoint(models[settings.dof.modelIndex], evaluatedModels_, settings.dof))
                    ImGui::TextWrapped("%s", ui::tr("Focus target unavailable; using the camera target."));
            }
            if ((settings.dof.focusMode == PreviewFocusMode::bone || settings.dof.focusMode == PreviewFocusMode::face ||
                 settings.dof.focusMode == PreviewFocusMode::eyes ||
                 settings.dof.focusMode == PreviewFocusMode::trackedSurface) &&
                ImGui::TreeNode(ui::label("Focus tracking"))) {
                ImGui::TextWrapped("%s", ui::tr("Local offset (m)"));
                changed |= ImGui::InputFloat3("##focus-local-offset", settings.dof.localOffsetMetres.data(), "%.3f");
                if (settings.dof.focusMode == PreviewFocusMode::face) {
                    ImGui::TextWrapped("%s", ui::tr("Face anchor offset (m)"));
                    changed |= ImGui::InputFloat3("##face-anchor-offset", settings.dof.faceOffsetMetres.data(), "%.3f");
                }
                lensSlider("AF response (s)", settings.dof.focusResponseSeconds, 0.0F, 1.0F, "%.3f");
                lensSlider("AF deadband (m)", settings.dof.focusDeadbandMetres, 0.0F, 0.05F, "%.3f");
                lensSlider("AF maximum speed (m/s)", settings.dof.focusMaxSpeed, 0.05F, 100.0F, "%.2f",
                           ImGuiSliderFlags_Logarithmic);
                ImGui::TreePop();
            }
            lensSlider("Focus offset (m)", settings.dof.focusOffsetMetres, -1.0F, 1.0F, "%.3f");
            const auto focusCamera = makeSceneCameraState();
            const PreviewFocusCamera focusView{focusCamera.rotation, focusCamera.position, focusCamera.distance,
                                               focusCamera.verticalFovRadians, focusCamera.perspective};
            const float focus = resolvePreviewFocusDistance(settings.dof, focusView, scene_.models(), evaluatedModels_);
            const auto lens =
                previewDofLens(settings.dof, focus, focusCamera.verticalFovRadians, 1080, PreviewDofQuality::balanced);
            ImGui::Text("%s: %.3f m", ui::tr("Resolved focus distance"),
                        static_cast<double>(previewFocusState_.currentMetres));
            ImGui::Text("%s: %.3f m", ui::tr("Target focus distance"), static_cast<double>(lens.focusMetres));
            ImGui::Text("%s: %.3f m/s", ui::tr("Focus velocity"), static_cast<double>(previewFocusState_.velocity));
            if (ImGui::Button(ui::label("Reset lens and focus"))) {
                settings.dof = {};
                changed = true;
            }
            if (ImGui::TreeNode(ui::label("Aperture shape"))) {
                int bladeChoice =
                    settings.dof.apertureBlades == 0 ? 0 : static_cast<int>(settings.dof.apertureBlades) - 4;
                ImGui::TextWrapped("%s", ui::tr("Aperture blades"));
                if (ImGui::Combo("##aperture-blades", &bladeChoice, ui::items("Circle|5|6|7|8|9|10|11|12"))) {
                    settings.dof.apertureBlades = bladeChoice == 0 ? 0U : static_cast<std::uint32_t>(bladeChoice + 4);
                    changed = true;
                }
                if (settings.dof.apertureBlades != 0) {
                    lensSlider("Blade curvature", settings.dof.apertureCurvature, 0.0F, 1.0F, "%.2f");
                    lensSlider("Aperture rotation", settings.dof.apertureRotationDegrees, -180.0F, 180.0F, "%.1f");
                }
                ImGui::TreePop();
            }
            if (ImGui::TreeNode(ui::label("Blur quality and export"))) {
                changed |= ImGui::Checkbox(ui::label("Temporal DoF (Balanced / High)"), &settings.dof.temporal);
                ImGui::TextWrapped("%s", ui::tr("Cinematic lens samples (image exports)"));
                int cinematic = settings.dof.cinematicSamples == 0     ? 0
                                : settings.dof.cinematicSamples == 64  ? 1
                                : settings.dof.cinematicSamples == 128 ? 2
                                                                       : 3;
                if (ImGui::Combo("##cinematic-samples", &cinematic, ui::items("Off|64|128|256"))) {
                    settings.dof.cinematicSamples = cinematic == 0   ? 0U
                                                    : cinematic == 1 ? 64U
                                                    : cinematic == 2 ? 128U
                                                                     : 256U;
                    changed = true;
                }
                if (settings.dof.cinematicSamples != 0)
                    ImGui::TextWrapped(
                        "%s",
                        ui::tr("Cinematic images use ACES; exposure, white balance and Bloom are not reapplied."));
                lensSlider("Radius cap at 1080p (px; 0 = Auto)", settings.dof.maxRadiusPixels, 0.0F, 128.0F, "%.1f");
                ImGui::TreePop();
            }
            if (ImGui::TreeNode(ui::label("Lens calibration"))) {
                lensSlider("Sensor height (mm)", settings.dof.sensorHeightMm, 1.0F, 100.0F, "%.1f");
                lensSlider("Metres per preview unit", settings.dof.metresPerUnit, 0.01F, 10.0F, "%.3f",
                           ImGuiSliderFlags_Logarithmic);
                ImGui::TextWrapped("%s", ui::tr("A two-unit character is 1.6 m tall at the default scale."));
                ImGui::TreePop();
            }
            ImGui::PopItemWidth();
            ImGui::TreePop();
        }
        ImGui::TextWrapped("%s", ui::tr("Ctrl + click: static focus. Ctrl + Shift + click: track the surface."));
        if (trackingFocusFallback_)
            ImGui::TextWrapped("%s", ui::tr("No bone anchor found; using static focus."));
        if (resolved.depthOfField && scene_.background().enabled &&
            scene_.background().screenSource == core::ScreenTextureSource::previousFrame)
            ImGui::TextWrapped("%s", ui::tr("Choose White, Image or Video background to preview depth of field."));
        if (!makeSceneCameraState().perspective)
            ImGui::TextWrapped("%s", ui::tr("Depth of field requires a perspective camera."));
        combo(ui::label("Anti-aliasing"), settings.antiAliasing, ui::items("Off|FXAA|Auto"));
        combo(ui::label("View transform"), settings.viewTransform, ui::items("ACES|AgX|Neutral"));
        changed |= ImGui::SliderFloat(ui::label("Exposure (EV)"), &settings.exposure, -8.0F, 8.0F, "%.2f");
        changed |=
            ImGui::SliderFloat(ui::label("White balance (K)"), &settings.whiteBalance, 1000.0F, 15000.0F, "%.0f");
    }
    if (ImGui::CollapsingHeader(ui::label("Render performance"))) {
        const auto timings = device_->previewPassTimings();
        if (device_->previewGpuNanoseconds() == 0) {
            ImGui::TextUnformatted(ui::tr("GPU timing unavailable"));
        } else {
            const auto timing = [](const char* name, std::uint64_t nanoseconds) {
                ImGui::Text("%s: %.2f ms", name, static_cast<double>(nanoseconds) / 1e6);
            };
            timing(ui::tr("Shadows"), timings.shadow);
            timing(ui::tr("AO / depth prepass"), timings.ambientOcclusion);
            timing(ui::tr("Materials"), timings.model);
            timing(ui::tr("Skin scattering"), timings.subsurface);
            timing(ui::tr("Circle of confusion"), timings.coc);
            timing(ui::tr("DoF downsample / tiles"), timings.dofDownsample);
            timing(ui::tr("DoF near / far blur"), timings.dofBlur);
            timing(ui::tr("DoF composite"), timings.dofComposite);
            timing(ui::tr("Bloom"), timings.postProcess);
            timing(ui::tr("Tone mapping"), timings.toneMap);
            timing(ui::tr("Total"), device_->previewGpuNanoseconds());
        }
    }
    if (changed) {
        previewFocusState_.trackingValid = false;
        if (settings.dof.modelIndex != previewMissingFocusModel)
            missingFocusModelProjectIndex_.reset();
        device_->cancelPreviewFocusPick();
        device_->setPreviewRenderSettings(settings);
        refreshPreviewScene();
    }
#endif
}

void Application::buildInspectorPanel() {
#if DAYO_HAS_IMGUI
    if (!uiState_.inspectorVisible)
        return;
    auto* model = selectedModel();
    timelineEditor_.synchronize(scene_, model != nullptr ? model->id : core::ModelId{}, editGlobalMotion_);
    if (inspectorFocusRequested_) {
        ImGui::SetNextWindowFocus();
        inspectorFocusRequested_ = false;
    }
    if (!ImGui::Begin(workspaceWindowName("Inspector", "inspector").c_str())) {
        ImGui::End();
        return;
    }
    if (ImGui::RadioButton(ui::label("Selection"), inspectorPage_ == InspectorPage::selection))
        inspectorPage_ = InspectorPage::selection;
    ImGui::SameLine();
    if (ImGui::RadioButton(ui::label("Camera"), inspectorPage_ == InspectorPage::camera))
        inspectorPage_ = InspectorPage::camera;
    ImGui::SameLine();
    if (ImGui::RadioButton(ui::label("Light"), inspectorPage_ == InspectorPage::light))
        inspectorPage_ = InspectorPage::light;
    ImGui::SameLine();
    if (ImGui::RadioButton(ui::label("Effects"), inspectorPage_ == InspectorPage::effects))
        inspectorPage_ = InspectorPage::effects;
    ImGui::Separator();
    if (inspectorPage_ == InspectorPage::effects) {
        buildRenderSettingsControls();
        ImGui::End();
        return;
    }
    if (inspectorPage_ == InspectorPage::selection && timelineEditor_.drawInspector(scene_, history_)) {
        playing_ = false;
        if (editGlobalMotion_) {
            manualCamera_ = cameraEditPreview_ = lightEditPreview_ = false;
            followVmdCamera_ = true;
        }
        const auto& selection = timelineEditor_.selection();
        if (selection.size() == 1) {
            if (const auto selectedFrame = ui::keyFrame(
                    *scene_.motion(model != nullptr ? model->id : 0, editGlobalMotion_), selection.front()))
                seekTimeline(static_cast<float>(*selectedFrame));
        } else {
            seekTimeline(scene_.timeline().frame);
        }
    }
    if (inspectorPage_ == InspectorPage::selection && !timelineEditor_.selection().empty()) {
        if (ImGui::Button(ui::label("Clear key selection"))) {
            timelineEditor_.clearSelection();
        } else {
            ImGui::End();
            return;
        }
    }
    if (inspectorPage_ != InspectorPage::selection) {
        buildCameraLightControls();
        ImGui::End();
        return;
    }
    if (uiState_.inspectScene || model == nullptr || model->model == nullptr) {
        ImGui::TextUnformatted(ui::tr("Scene settings"));
        if (ImGui::CollapsingHeader(ui::label("Scene"), ImGuiTreeNodeFlags_DefaultOpen)) {
            auto background = scene_.background();
            int source = static_cast<int>(background.screenSource);
            if (ImGui::BeginTable("scene-properties", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn(ui::label("Property"), ImGuiTableColumnFlags_WidthFixed,
                                        ImGui::GetFontSize() * 10.0F);
                ImGui::TableSetupColumn(ui::label("Value"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(ui::tr("Background"));
                ImGui::TableSetColumnIndex(1);
                if (ImGui::Combo("##background-source", &source, ui::items("Previous frame|Video|Image|White"))) {
                    selectBackgroundSource(static_cast<core::ScreenTextureSource>(source));
                }
                ImGui::EndTable();
            }
            bool enabled = background.enabled;
            if (ImGui::Checkbox(ui::label("Enabled"), &enabled)) {
                scene_.setBackgroundEnabled(enabled);
                refreshPreviewScene();
            }
            buildMediaBackgroundUi();
            ImGui::SeparatorText(ui::tr("Preview lighting"));
            ImGui::InputText(ui::label("HDRI file"), previewHdriPath_.data(), previewHdriPath_.size());
            if (ImGui::Button(ui::label("Apply lighting HDRI"))) {
                projectEditorState_.skyboxFile = previewHdriPath_.data();
                failedPreviewHdriPath_.clear();
                scene_.markDirty(core::DirtyFlag::lighting);
            }
            ImGui::SameLine();
            if (ImGui::Button(ui::label("Use background lighting"))) {
                projectEditorState_.skyboxFile.clear();
                previewHdriPath_.fill(0);
                failedPreviewHdriPath_.clear();
                scene_.markDirty(core::DirtyFlag::lighting);
            }
            if (!previewHdriError_.empty())
                ImGui::TextWrapped(ui::tr("Lighting HDRI: %s"), previewHdriError_.c_str());
        }
        ImGui::End();
        return;
    }

    ImGui::Text(ui::tr("Model: %s"), model->displayName.c_str());
    ImGui::TextDisabled(ui::tr("PMX  ·  %zu bones  ·  %zu morphs"), model->model->bones.size(),
                        model->model->morphs.size());
    ImGui::Separator();
    if (ImGui::CollapsingHeader(ui::label("Model"), uiState_.workspace == ui::Workspace::animation
                                                        ? ImGuiTreeNodeFlags_None
                                                        : ImGuiTreeNodeFlags_DefaultOpen)) {
        bool visible = model->visible;
        if (ImGui::Checkbox(ui::label("Visible"), &visible)) {
            scene_.setModelVisible(model->id, visible);
            refreshAnimatedMesh(true);
        }
        int clones = static_cast<int>(model->cloneCount);
        if (ImGui::DragInt(ui::label("Clone count"), &clones, 1.0F, 1, 16)) {
            scene_.setCloneCount(model->id, static_cast<std::uint32_t>(clones));
            refreshAnimatedMesh(true);
        }
        ImGui::TextDisabled(ui::tr("Source: %s"), model->sourcePath.filename().string().c_str());
        ImGui::TextDisabled("%s", model->model->metadata.comment.c_str());
    }
    if (ImGui::CollapsingHeader(ui::label("Bone / Morph"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto& bones = model->model->bones;
        if (!bones.empty()) {
            selectedBone_ = std::clamp(selectedBone_, 0, static_cast<int>(bones.size() - 1));
            if (ImGui::BeginCombo(ui::label("Bone##InspectorBoneSelector"),
                                  bones[static_cast<std::size_t>(selectedBone_)].name.c_str())) {
                for (std::size_t index = 0; index < bones.size(); ++index) {
                    ImGui::PushID(static_cast<int>(index));
                    if (ImGui::Selectable(bones[index].name.c_str(), selectedBone_ == static_cast<int>(index)))
                        selectedBone_ = static_cast<int>(index);
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::DragFloat3(ui::label("Position"), editedBoneTranslation_.data(), 0.01F);
            ImGui::DragFloat4(ui::label("Rotation"), editedBoneRotation_.data(), 0.01F, -1.0F, 1.0F);
            ImGui::Checkbox(ui::label("Physics"), &editedBonePhysics_);
            if (ImGui::Button(ui::label("Register Bone Key"))) {
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
            if (ImGui::BeginCombo(ui::label("Morph##InspectorMorphSelector"),
                                  morphs[static_cast<std::size_t>(selectedMorph_)].name.c_str())) {
                for (std::size_t index = 0; index < morphs.size(); ++index) {
                    ImGui::PushID(static_cast<int>(index));
                    if (ImGui::Selectable(morphs[index].name.c_str(), selectedMorph_ == static_cast<int>(index)))
                        selectedMorph_ = static_cast<int>(index);
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            const auto morphUi = core::fx::resolveFxMorphControllerUi(
                scene_.effects(), *model, morphs[static_cast<std::size_t>(selectedMorph_)].name);
            drawMorphWeightControl(editedMorphWeight_, morphUi);
            if (ImGui::Button(ui::label("Register Morph Key"))) {
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
    if (ImGui::CollapsingHeader(ui::label("Material"), uiState_.workspace == ui::Workspace::animation
                                                           ? ImGuiTreeNodeFlags_None
                                                           : ImGuiTreeNodeFlags_DefaultOpen)) {
        if (model->model->materials.empty()) {
            ImGui::TextDisabled("%s", ui::tr("No materials"));
        } else {
            int material = uiState_.selectedMaterial >= 0 &&
                                   uiState_.selectedMaterial < static_cast<std::int32_t>(model->model->materials.size())
                               ? uiState_.selectedMaterial
                               : 0;
            if (ImGui::BeginCombo(ui::label("Material##InspectorMaterialSelector"),
                                  model->model->materials[static_cast<std::size_t>(material)].name.c_str())) {
                for (std::size_t index = 0; index < model->model->materials.size(); ++index) {
                    ImGui::PushID(static_cast<int>(index));
                    if (ImGui::Selectable(model->model->materials[index].name.c_str(),
                                          material == static_cast<int>(index))) {
                        uiState_.selectedMaterial = static_cast<std::int32_t>(index);
                        material = static_cast<int>(index);
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            const auto& selectedMaterial = model->model->materials[static_cast<std::size_t>(material)];
            ImGui::Text(ui::tr("Diffuse %.2f  Specular %.2f  Edge %.3f"), selectedMaterial.diffuse[0],
                        selectedMaterial.specular[0], selectedMaterial.edgeSize);
            ImGui::TextWrapped(
                ui::tr("Base: %s"),
                selectedMaterial.textureIndex >= 0 &&
                        static_cast<std::size_t>(selectedMaterial.textureIndex) < model->model->textures.size()
                    ? mmd::pmx::resolveTexturePath(*model->model,
                                                   static_cast<std::size_t>(selectedMaterial.textureIndex))
                          .filename()
                          .string()
                          .c_str()
                    : ui::tr("none"));
            if (static_cast<std::size_t>(material) < model->materialSettings.size()) {
                int preset = model->materialSettings[static_cast<std::size_t>(material)].previewPbrPreset;
                if (ImGui::Combo(ui::label("Preview material"), &preset,
                                 ui::items("PMX|Skin|Hair|Cloth|Metal|Plastic|Glass"))) {
                    model->materialSettings[static_cast<std::size_t>(material)].previewPbrPreset =
                        static_cast<std::uint8_t>(preset);
                    scene_.markDirty(core::DirtyFlag::material);
                    refreshAnimatedMesh(false);
                }
            }
            if (ImGui::Checkbox(ui::label("Enable PMX outlines (preview)"), &previewOutlineEnabled_))
                refreshPreviewScene();
        }
    }
    if (uiState_.workspace == ui::Workspace::debug &&
        ImGui::CollapsingHeader(ui::label("Evaluation Order (experimental)"))) {
        ImGui::TextDisabled(
            "%s", ui::tr("Motion order drives animation evaluation. Other order fields are persisted; runtime "
                         "scheduling support is partial."));
        if (ImGui::DragInt(ui::label("Motion"), &model->order.motion, 1.0F, 0, 1024))
            scene_.markDirty(core::DirtyFlag::geometry);
        ImGui::BeginDisabled();
        ImGui::DragInt(ui::label("Deform"), &model->order.deform, 1.0F, 0, 1024);
        ImGui::DragInt(ui::label("Postprocess"), &model->order.postprocess, 1.0F, 0, 1024);
        ImGui::DragInt(ui::label("Raster"), &model->order.raster, 1.0F, 0, 1024);
        ImGui::EndDisabled();
    }
    ImGui::End();
#endif
}

void Application::buildSaveAsDialog() {
#if DAYO_HAS_IMGUI
    if (uiState_.saveAsOpen) {
        ImGui::OpenPopup(ui::label("Save Project As"));
        uiState_.saveAsOpen = false;
    }
    if (!ImGui::BeginPopupModal(ui::label("Save Project As"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextUnformatted(ui::tr("Choose a Dayo project destination."));
    ImGui::InputText(ui::label("Path"), projectDestination_.data(), projectDestination_.size());
    if (ImGui::Button(ui::label("Save"))) {
        saveProjectAsNow();
        if (projectSaveStatus_ == "Project saved")
            ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(ui::label("Cancel")))
        ImGui::CloseCurrentPopup();
    if (!projectSaveStatus_.empty())
        ImGui::TextWrapped("%s", ui::tr(projectSaveStatus_.c_str()));
    ImGui::EndPopup();
#endif
}

void Application::drainVideoReadbacks() {
    if (device_ != nullptr) {
        for (auto ticket : videoReadbacks_)
            static_cast<void>(device_->collectRenderedImage(ticket));
    }
    videoReadbacks_.clear();
}

void Application::restoreVideoExportState() {
    drainVideoReadbacks();
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
        restartAudioAtCurrentFrame();
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
        sequenceOutput_.sequenceFile = sequenceOutputFilename_.data();
        core::OutputQueue output(sequenceOutput_);
        imageSequenceRestoreFrame_ = scene_.timeline().frame;
        imageSequenceRestoreMediaSeconds_ = mediaSeconds_;
        imageSequenceRestorePlaying_ = playing_;
        imageSequenceRestoreManualCamera_ = manualCamera_;
        imageSequenceNextFrame_ = sequenceOutput_.firstFrame;
        imageSequenceSampleIndex_ = 0;
        imageSequenceSampleCount_ = std::max(sequenceOutput_.samples, std::uint32_t{1});
        imageSequenceLensSamples_.clear();
        auto outputSettings = device_->previewRenderSettings();
        if (outputSettings.preset == graphics::PreviewQualityPreset::automatic)
            outputSettings.preset = graphics::PreviewQualityPreset::high;
        const auto outputEffects = graphics::resolvePreviewEffects(outputSettings, graphics::PreviewQuality::apu, true,
                                                                   false, device_->previewEffectSupport());
        if (device_->activeRenderer() == graphics::RendererKind::preview && outputSettings.dof.cinematicSamples != 0 &&
            outputEffects.depthOfField && outputEffects.depthOfFieldIntensity > 0.0F) {
            imageSequenceSampleCount_ = std::max(imageSequenceSampleCount_, outputSettings.dof.cinematicSamples);
            imageSequenceLensSamples_ = graphics::previewApertureSamples(
                outputSettings.dof.apertureBlades, outputSettings.dof.apertureCurvature,
                outputSettings.dof.apertureRotationDegrees, imageSequenceSampleCount_);
        }
        imageSequencePreviousSampleFrame_ = static_cast<float>(sequenceOutput_.firstFrame);
        imageSequenceFramesFinished_ = false;
        imageSequencePreRollDone_ = sequenceOutput_.firstFrame == 0U;
        imageSequencePreRollFrame_ = imageSequencePreRollDone_ ? 0U : 1U;
        imageSequenceRestoring_ = false;
        imageSequenceCancelRequested_ = false;
        imageSequenceCompletionStatus_.clear();
        imageSequenceImage_ = {};
        imageSequenceHdrSamples_ = {};
        stateMutationStarted = true;
        resetPhysicsSimulation();
        evaluateExportFrame(0.0F, 0.0F, true);
        imageSequenceOutput_.emplace(std::move(output));
        imageSequenceExportRunning_ = true;
        nativeOnStartPending_ = true;
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
    restartAudioAtCurrentFrame();
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
    if (device_ != nullptr)
        device_->setPreviewLensSample(std::nullopt);
    if (device_ != nullptr)
        device_->setPreviewJitter(0.0F, 0.0F);
    if (device_ != nullptr)
        device_->setPreviewStillQuality(false);
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
    imageSequenceHdrSamples_ = {};
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

        const bool previewRender = device_->activeRenderer() == graphics::RendererKind::preview;
        const float previewScale = previewStillScale_ == 2 ? 2.0F : (previewStillScale_ == 1 ? 4.0F / 3.0F : 1.0F);
        const std::uint32_t renderWidth =
            previewRender ? static_cast<std::uint32_t>(std::ceil(static_cast<float>(sequenceWidth_) * previewScale))
                          : sequenceWidth_;
        const std::uint32_t renderHeight =
            previewRender ? static_cast<std::uint32_t>(std::ceil(static_cast<float>(sequenceHeight_) * previewScale))
                          : sequenceHeight_;
        if (previewRender && imageSequenceSampleCount_ > 1U) {
            const auto sample = imageSequenceSampleIndex_ + 1U;
            device_->setPreviewJitter(halton(sample, 2U) - 0.5F, halton(sample, 3U) - 0.5F);
        }
        if (previewRender) {
            device_->setPreviewStillQuality(true);
            if (!imageSequenceLensSamples_.empty())
                device_->setPreviewLensSample(imageSequenceLensSamples_[imageSequenceSampleIndex_]);
        }
        auto rendered = device_->renderToImage({renderWidth, renderHeight});
        if (previewRender) {
            device_->setPreviewLensSample(std::nullopt);
            device_->setPreviewJitter(0.0F, 0.0F);
            device_->setPreviewStillQuality(false);
        }
        const auto finishOutputFrame = [&]() {
            if (!imageSequenceOutput_->tryPush(frame, std::move(imageSequenceImage_)))
                throw std::runtime_error("image output queue unexpectedly full");
            imageSequenceHdrSamples_ = {};
            imageSequenceSampleIndex_ = 0;
            if (frame == sequenceOutput_.lastFrame)
                imageSequenceFramesFinished_ = true;
            else
                ++imageSequenceNextFrame_;
        };

        if (!previewRender) {
            if (imageSequenceSampleIndex_ + 1U < imageSequenceSampleCount_) {
                ++imageSequenceSampleIndex_;
                return;
            }
            imageSequenceImage_ = std::move(rendered);
            finishOutputFrame();
            return;
        }

        const auto hdrTexture = device_->previewHdrTexture();
        if (!hdrTexture.valid())
            throw std::runtime_error("preview HDR output is unavailable");
        const auto hdrSample = device_->readbackTextureEx(hdrTexture, 0U, 0U);
        if (imageSequenceSampleIndex_ == 0U)
            imageSequenceHdrSamples_.begin({renderWidth, renderHeight, 1U}, imageSequenceSampleCount_);
        imageSequenceHdrSamples_.add(hdrSample);
        ++imageSequenceSampleIndex_;
        if (imageSequenceSampleIndex_ < imageSequenceSampleCount_)
            return;

        const auto averaged = imageSequenceHdrSamples_.resolveFloat32();
        std::vector<std::uint8_t> averagedHdr(static_cast<std::size_t>(sequenceWidth_) * sequenceHeight_ * 8U);
        const float scaleX = static_cast<float>(renderWidth) / static_cast<float>(sequenceWidth_);
        const float scaleY = static_cast<float>(renderHeight) / static_cast<float>(sequenceHeight_);
        for (std::uint32_t y = 0; y < sequenceHeight_; ++y) {
            const float y0 = static_cast<float>(y) * scaleY;
            const float y1 = static_cast<float>(y + 1U) * scaleY;
            for (std::uint32_t x = 0; x < sequenceWidth_; ++x) {
                const float x0 = static_cast<float>(x) * scaleX;
                const float x1 = static_cast<float>(x + 1U) * scaleX;
                std::array<float, 4> color{};
                for (std::uint32_t sy = static_cast<std::uint32_t>(y0);
                     sy < std::min(renderHeight, static_cast<std::uint32_t>(std::ceil(y1))); ++sy) {
                    const float wy = std::min(y1, static_cast<float>(sy + 1U)) - std::max(y0, static_cast<float>(sy));
                    for (std::uint32_t sx = static_cast<std::uint32_t>(x0);
                         sx < std::min(renderWidth, static_cast<std::uint32_t>(std::ceil(x1))); ++sx) {
                        const float wx =
                            std::min(x1, static_cast<float>(sx + 1U)) - std::max(x0, static_cast<float>(sx));
                        const auto source = (static_cast<std::size_t>(sy) * renderWidth + sx) * 4U;
                        for (std::size_t channel = 0; channel < 4; ++channel)
                            color[channel] += averaged[source + channel] * wx * wy;
                    }
                }
                const float denominator = scaleX * scaleY;
                const auto destination = (static_cast<std::size_t>(y) * sequenceWidth_ + x) * 8U;
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    const auto half = core::floatToHalf(color[channel] / denominator);
                    std::memcpy(averagedHdr.data() + destination + channel * sizeof(half), &half, sizeof(half));
                }
            }
        }
        imageSequenceImage_ = device_->renderPreviewHdrToImage({sequenceWidth_, sequenceHeight_}, averagedHdr);
        finishOutputFrame();
    } catch (const std::exception& error) {
        finishImageSequenceExport(error.what());
    }
#endif
}

void Application::buildImageSequenceExportUi() {
#if DAYO_HAS_IMGUI
    if (uiState_.imageSequenceExportOpen) {
        ImGui::OpenPopup(ui::label("Export Image Sequence"));
        uiState_.imageSequenceExportOpen = false;
    }
    if (!ImGui::BeginPopupModal(ui::label("Export Image Sequence"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    if (imageSequenceExportRunning_) {
        if (imageSequenceRestoring_) {
            ImGui::TextUnformatted(ui::tr("Restoring preview state..."));
        } else {
            const auto total = static_cast<std::uint64_t>(sequenceOutput_.lastFrame) -
                               static_cast<std::uint64_t>(sequenceOutput_.firstFrame) + 1U;
            const auto completed =
                imageSequencePreRollDone_
                    ? (imageSequenceFramesFinished_ ? total
                                                    : static_cast<std::uint64_t>(imageSequenceNextFrame_) -
                                                          static_cast<std::uint64_t>(sequenceOutput_.firstFrame))
                    : 0U;
            ImGui::Text(ui::tr("Rendering %u / %u"),
                        imageSequenceFramesFinished_ ? sequenceOutput_.lastFrame : imageSequenceNextFrame_,
                        sequenceOutput_.lastFrame);
            ImGui::ProgressBar(total == 0U ? 0.0F : static_cast<float>(completed) / static_cast<float>(total),
                               {-1.0F, 0.0F});
            ImGui::Text(ui::tr("Current sample: %u / %u"), imageSequenceSampleIndex_ + 1U, imageSequenceSampleCount_);
            if (ImGui::Button(ui::label("Cancel")))
                imageSequenceCancelRequested_ = true;
        }
    } else {
        ImGui::InputText(ui::label("Directory"), sequenceOutputDirectory_.data(), sequenceOutputDirectory_.size());
        if (ImGui::InputText(ui::label("First filename"), sequenceOutputFilename_.data(),
                             sequenceOutputFilename_.size())) {
            const auto extension = std::filesystem::path(sequenceOutputFilename_.data()).extension().string();
            if (extension == ".png")
                sequenceOutput_.format = core::OutputFormat::png;
            else if (extension == ".exr")
                sequenceOutput_.format = core::OutputFormat::exr;
            else if (extension == ".ppm")
                sequenceOutput_.format = core::OutputFormat::ppm;
        }
        int first = static_cast<int>(sequenceOutput_.firstFrame);
        int last = static_cast<int>(sequenceOutput_.lastFrame);
        int samples = static_cast<int>(sequenceOutput_.samples);
        if (ImGui::InputInt(ui::label("First frame"), &first)) {
            sequenceOutput_.firstFrame = static_cast<std::uint32_t>(std::max(first, 0));
            projectEditorState_.recordStart = static_cast<std::int32_t>(sequenceOutput_.firstFrame);
        }
        if (ImGui::InputInt(ui::label("Last frame"), &last)) {
            sequenceOutput_.lastFrame = static_cast<std::uint32_t>(std::max(last, 0));
            projectEditorState_.recordEnd = static_cast<std::int32_t>(sequenceOutput_.lastFrame);
        }
        if (ImGui::InputInt(ui::label("Samples"), &samples))
            sequenceOutput_.samples = static_cast<std::uint32_t>(std::clamp(samples, 1, 4096));
        ImGui::Combo(ui::label("Preview render scale"), &previewStillScale_, ui::items("1x|1.33x|2x"));
        int width = static_cast<int>(sequenceWidth_);
        int height = static_cast<int>(sequenceHeight_);
        if (ImGui::InputInt(ui::label("Width"), &width)) {
            sequenceWidth_ = static_cast<std::uint32_t>(std::max(width, 1));
            sequencePreset_ = 0;
        }
        if (ImGui::InputInt(ui::label("Height"), &height)) {
            sequenceHeight_ = static_cast<std::uint32_t>(std::max(height, 1));
            sequencePreset_ = 0;
        }
        if (ImGui::Combo(ui::label("Preset"), &sequencePreset_, ui::items("Custom|720p|1080p|1440p|4K"))) {
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
        ImGui::Checkbox(ui::label("Motion blur"), &sequenceOutput_.motionBlur);
        ImGui::Checkbox(ui::label("Overwrite existing frames"), &sequenceOutput_.overwrite);
        int format = sequenceOutput_.format == core::OutputFormat::png   ? 1
                     : sequenceOutput_.format == core::OutputFormat::exr ? 2
                                                                         : 0;
#if DAYO_HAS_OPENEXR
        constexpr int formatCount = 3;
        constexpr const char* formatNames = "PPM\0PNG\0OpenEXR\0";
#else
        constexpr int formatCount = 2;
        constexpr const char* formatNames = "PPM\0PNG\0";
        format = std::min(format, formatCount - 1);
#endif
        format = std::clamp(format, 0, formatCount - 1);
        if (ImGui::Combo(ui::label("Format"), &format, formatNames))
            sequenceOutput_.format = static_cast<core::OutputFormat>(format);
        try {
            auto previewSettings = sequenceOutput_;
            previewSettings.directory = sequenceOutputDirectory_.data();
            previewSettings.sequenceFile = sequenceOutputFilename_.data();
            ImGui::TextWrapped(ui::tr("First output: %s"),
                               core::firstSequenceOutputPath(previewSettings).string().c_str());
            ImGui::TextDisabled(
                "%s", ui::tr("Output numbers are independent of scene frames. Existing numbers are skipped unless "
                             "overwrite is enabled."));
        } catch (const std::exception& error) {
            ImGui::TextWrapped("%s", ui::tr(error.what()));
        }
        if (ImGui::Button(ui::label("Render sequence")))
            startImageSequenceExport();
    }
    if (!sequenceOutputStatus_.empty())
        ImGui::TextWrapped("%s", ui::tr(sequenceOutputStatus_.c_str()));
    ImGui::SameLine();
    if (!imageSequenceExportRunning_ && ImGui::Button(ui::label("Close")))
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
            ImGui::TableSetupColumn(ui::label("Status"), ImGuiTableColumnFlags_WidthStretch, 2.0F);
            ImGui::TableSetupColumn(ui::label("Model"), ImGuiTableColumnFlags_WidthStretch, 1.0F);
            ImGui::TableSetupColumn(ui::label("Frame"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9.0F);
            ImGui::TableSetupColumn(ui::label("FPS"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.0F);
            ImGui::TableSetupColumn(ui::label("Project"), ImGuiTableColumnFlags_WidthStretch, 1.5F);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const auto statusText = lastAsset_.text();
            ImGui::TextUnformatted(statusText.c_str());
            if (ImGui::IsItemHovered() && !lastAsset_.empty())
                ImGui::SetTooltip("%s", statusText.c_str());
            ImGui::TableSetColumnIndex(1);
            if (const auto* model = selectedModel())
                ImGui::TextUnformatted(model->displayName.c_str());
            else
                ImGui::TextDisabled("%s", ui::tr("Scene"));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text(ui::tr("Frame %.0f / %.0f"), animationFrame_, scene_.timeline().duration);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text(ui::tr("%.0f FPS"), ImGui::GetIO().Framerate);
            ImGui::TableSetColumnIndex(4);
            const std::string projectText =
                currentProjectPath_ ? currentProjectPath_->string() : ui::tr("Untitled project");
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
    const auto target = model != nullptr ? model->id : core::ModelId{};
    const auto execute = [&](core::VmdMotion before, core::MotionDocument document, bool globalMotion,
                             std::string label) {
        const auto name = before.modelName.empty() && model != nullptr ? model->displayName : before.modelName;
        history_.execute(scene_, std::make_unique<core::EditMotionCommand>(target, globalMotion, std::move(before),
                                                                           core::toVmdMotion(std::move(document), name),
                                                                           std::move(label)));
        timelineEditor_.clearSelection();
        refreshAnimatedMesh(false);
        refreshPreviewScene();
    };
    const bool debugWorkspace = uiState_.workspace == ui::Workspace::debug;
    if (uiState_.timelineVisible && ImGui::Begin(workspaceWindowName("Timeline", "timeline").c_str(), nullptr,
                                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        uiState_.timelineFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (ImGui::Button("|<"))
            seekTimeline(0);
        ImGui::SameLine();
        if (ImGui::Button("<"))
            seekTimeline(std::max(0.0F, animationFrame_ - 1));
        ImGui::SameLine();
        if (ImGui::Button(playing_ ? ui::label("Pause") : ui::label("Play"))) {
            playing_ = !playing_;
            restartAudioAtCurrentFrame();
        }
        ImGui::SameLine();
        if (ImGui::Button(">"))
            seekTimeline(std::min(scene_.timeline().duration, animationFrame_ + 1));
        ImGui::SameLine();
        if (ImGui::Button(">|"))
            seekTimeline(scene_.timeline().duration);
        ImGui::SameLine();
        int frame = static_cast<int>(std::clamp(static_cast<double>(animationFrame_), 0.0,
                                                static_cast<double>(std::numeric_limits<int>::max())));
        ImGui::SetNextItemWidth(85);
        if (ImGui::InputInt(ui::label("Frame"), &frame, 0)) {
            playing_ = false;
            seekTimeline(std::clamp(static_cast<float>(frame), 0.0F, scene_.timeline().duration));
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(ui::label("Model"), !editGlobalMotion_))
            editGlobalMotion_ = false;
        ImGui::SameLine();
        if (ImGui::RadioButton(ui::label("Camera / light"), editGlobalMotion_))
            editGlobalMotion_ = true;
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(ui::tr("Playback / media")).x + 20.0F)
            ImGui::NewLine();
        if (ImGui::Button(ui::label("Playback / media")))
            ImGui::OpenPopup("timeline-playback");
        if (ImGui::BeginPopup("timeline-playback")) {
            ImGui::Checkbox(ui::label("Loop"), &repeat_);
            ImGui::SetNextItemWidth(100);
            if (ImGui::DragFloat(ui::label("Speed"), &playbackSpeed_, .01F, .1F, 4, "%.2fx"))
                restartAudioAtCurrentFrame();
            ImGui::SeparatorText(ui::tr("Media"));
            buildMediaPlaybackControls(true);
            if (!waveformPeaks_.empty())
                ImGui::PlotLines(ui::label("Audio"), waveformPeaks_.data(), static_cast<int>(waveformPeaks_.size()), 0,
                                 nullptr, 0, 1, {300, 44});
            ImGui::EndPopup();
        }
        timelineEditor_.synchronize(scene_, target, editGlobalMotion_);
        const auto result = timelineEditor_.draw(scene_, history_, animationFrame_);
        if (result.changed && editGlobalMotion_) {
            manualCamera_ = cameraEditPreview_ = lightEditPreview_ = false;
            followVmdCamera_ = true;
        }
        if (result.pause && playing_) {
            playing_ = false;
            restartAudioAtCurrentFrame();
        }
        if (result.seek)
            seekTimeline(*result.seek);
        else if (result.changed) {
            seekTimeline(scene_.timeline().frame);
        }
        if (result.seek && !timelineEditor_.selection().empty()) {
            inspectorPage_ = InspectorPage::selection;
            uiState_.inspectScene = false;
            if (!uiState_.inspectorVisible) {
                uiState_.inspectorVisible = true;
                uiState_.resetLayoutRequested = true;
            }
        }
    }
    if (uiState_.timelineVisible)
        ImGui::End();

    if (debugWorkspace && ImGui::Begin(workspaceWindowName("Bone / Expression", "legacy-bone").c_str())) {
        if (model == nullptr || model->model == nullptr) {
            ImGui::TextUnformatted(ui::tr("Select a model to edit bones and morphs."));
        } else {
            const auto& bones = model->model->bones;
            if (!bones.empty()) {
                selectedBone_ = std::clamp(selectedBone_, 0, static_cast<int>(bones.size() - 1));
                if (ImGui::BeginCombo(ui::label("Bone##LegacyBoneSelector"),
                                      bones[static_cast<std::size_t>(selectedBone_)].name.c_str())) {
                    for (std::size_t i = 0; i < bones.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::Selectable(bones[i].name.c_str(), selectedBone_ == static_cast<int>(i)))
                            selectedBone_ = static_cast<int>(i);
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                ImGui::DragFloat3(ui::label("Translation"), editedBoneTranslation_.data(), 0.01F);
                ImGui::DragFloat4(ui::label("Rotation quaternion"), editedBoneRotation_.data(), 0.01F, -1.0F, 1.0F);
                ImGui::Checkbox(ui::label("Bone physics"), &editedBonePhysics_);
                if (ImGui::Button(ui::label("Register bone"))) {
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
                if (ImGui::BeginCombo(ui::label("Morph##LegacyMorphSelector"),
                                      morphs[static_cast<std::size_t>(selectedMorph_)].name.c_str())) {
                    for (std::size_t i = 0; i < morphs.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::Selectable(morphs[i].name.c_str(), selectedMorph_ == static_cast<int>(i)))
                            selectedMorph_ = static_cast<int>(i);
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                const auto morphUi = core::fx::resolveFxMorphControllerUi(
                    scene_.effects(), *model, morphs[static_cast<std::size_t>(selectedMorph_)].name);
                drawMorphWeightControl(editedMorphWeight_, morphUi);
                if (ImGui::Button(ui::label("Register morph"))) {
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

    if (debugWorkspace && ImGui::Begin(workspaceWindowName("Project tools", "legacy-project").c_str())) {
        ImGui::InputText(ui::label("Project file"), projectDestination_.data(), projectDestination_.size());
        if (ImGui::Button(ui::label("Save Dayo 1.30 project")))
            saveProjectAsNow();
        if (!projectSaveStatus_.empty())
            ImGui::TextWrapped("%s", ui::tr(projectSaveStatus_.c_str()));
        ImGui::Separator();
        ImGui::TextUnformatted(ui::tr("Preview lighting HDRI (2:1 equirectangular)"));
        ImGui::InputText(ui::label("HDRI file"), previewHdriPath_.data(), previewHdriPath_.size());
        if (ImGui::Button(ui::label("Apply lighting HDRI"))) {
            projectEditorState_.skyboxFile = previewHdriPath_.data();
            failedPreviewHdriPath_.clear();
            scene_.markDirty(core::DirtyFlag::lighting);
        }
        ImGui::SameLine();
        if (ImGui::Button(ui::label("Use background lighting"))) {
            projectEditorState_.skyboxFile.clear();
            previewHdriPath_.fill(0);
            failedPreviewHdriPath_.clear();
            scene_.markDirty(core::DirtyFlag::lighting);
        }
        if (!projectEditorState_.skyboxFile.empty())
            ImGui::TextWrapped(ui::tr("Active: %s"), projectEditorState_.skyboxFile.string().c_str());
        if (!previewHdriError_.empty())
            ImGui::TextWrapped(ui::tr("Lighting HDRI: %s"), previewHdriError_.c_str());
        ImGui::Separator();
        auto background = scene_.background();
        int source = static_cast<int>(background.screenSource);
        if (ImGui::Combo(ui::label("Background source"), &source, ui::items("Previous frame|Video|Image|White"))) {
            selectBackgroundSource(static_cast<core::ScreenTextureSource>(source));
        }
        buildMediaBackgroundUi();
        bool enabled = background.enabled;
        if (ImGui::Checkbox(ui::label("Background enabled"), &enabled)) {
            scene_.setBackgroundEnabled(enabled);
            refreshPreviewScene();
        }
        bool crop = background.crop == core::ScreenCropMode::crop4x3;
        if (ImGui::Checkbox(ui::label("Crop 4:3"), &crop)) {
            scene_.setBackgroundCrop(crop ? core::ScreenCropMode::crop4x3 : core::ScreenCropMode::none);
            refreshPreviewScene();
        }
        bool alpha = background.mode == core::BackgroundMode::alpha;
        if (ImGui::Checkbox(ui::label("Alpha background"), &alpha)) {
            scene_.setBackgroundMode(alpha ? core::BackgroundMode::alpha : core::BackgroundMode::opaque);
            refreshPreviewScene();
        }
        if (model != nullptr) {
            ImGui::SeparatorText(ui::tr("Model order"));
            ImGui::DragInt(ui::label("Motion order"), &model->order.motion, 1.0F, 0, 1024);
            ImGui::DragInt(ui::label("Deform order"), &model->order.deform, 1.0F, 0, 1024);
            ImGui::DragInt(ui::label("Postprocess order"), &model->order.postprocess, 1.0F, 0, 1024);
            ImGui::DragInt(ui::label("Raster order"), &model->order.raster, 1.0F, 0, 1024);
            if (ImGui::TreeNode(ui::label("Model description"))) {
                ImGui::TextWrapped("%s", model->model->metadata.comment.c_str());
                ImGui::TextUnformatted(model->sourcePath.parent_path().string().c_str());
                if (ImGui::Button(ui::label("Open model folder"))) {
                    const auto url = "file://" + model->sourcePath.parent_path().generic_string();
                    if (!SDL_OpenURL(url.c_str()))
                        lastAsset_ = ui::Message("Open folder: {0}", {SDL_GetError()});
                }
                ImGui::TreePop();
            }
            if (!model->materialSettings.empty() && ImGui::TreeNode(ui::label("Material settings"))) {
                static int materialIndex = 0;
                materialIndex = std::clamp(materialIndex, 0, static_cast<int>(model->materialSettings.size() - 1));
                const auto& materials = model->model->materials;
                const char* preview = materials[static_cast<std::size_t>(materialIndex)].name.c_str();
                if (ImGui::BeginCombo(ui::label("Material##MaterialAnnotationSelector"), preview)) {
                    for (std::size_t i = 0; i < materials.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::Selectable(materials[i].name.c_str(), materialIndex == static_cast<int>(i)))
                            materialIndex = static_cast<int>(i);
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                static std::array<char, 1024> annotation{};
                ImGui::InputText(ui::label("Annotation / MatDesc"), annotation.data(), annotation.size());
                if (ImGui::Button(ui::label("Apply material annotation"))) {
                    model->materialSettings[static_cast<std::size_t>(materialIndex)].annotation = annotation.data();
                    scene_.markDirty(core::DirtyFlag::material | core::DirtyFlag::effect);
                }
                int preset = model->materialSettings[static_cast<std::size_t>(materialIndex)].previewPbrPreset;
                if (ImGui::Combo(ui::label("Preview material"), &preset,
                                 ui::items("PMX|Skin|Hair|Cloth|Metal|Plastic|Glass"))) {
                    model->materialSettings[static_cast<std::size_t>(materialIndex)].previewPbrPreset =
                        static_cast<std::uint8_t>(preset);
                    scene_.markDirty(core::DirtyFlag::material);
                    refreshAnimatedMesh(false);
                }
                ImGui::TreePop();
            }
            if (scene_.models().size() > 1 && ImGui::TreeNode(ui::label("External parent"))) {
                static int parentIndex = 0;
                static std::array<char, 256> parentBone{};
                static std::array<char, 256> childBone{};
                parentIndex = std::clamp(parentIndex, 0, static_cast<int>(scene_.models().size() - 1));
                if (ImGui::BeginCombo(ui::label("Parent model##ExternalParentModelSelector"),
                                      scene_.models()[static_cast<std::size_t>(parentIndex)].displayName.c_str())) {
                    for (std::size_t i = 0; i < scene_.models().size(); ++i) {
                        ImGui::PushID(static_cast<int>(scene_.models()[i].id));
                        if (ImGui::Selectable(scene_.models()[i].displayName.c_str(),
                                              parentIndex == static_cast<int>(i)))
                            parentIndex = static_cast<int>(i);
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                ImGui::InputText(ui::label("Parent bone"), parentBone.data(), parentBone.size());
                ImGui::InputText(ui::label("Child bone"), childBone.data(), childBone.size());
                if (ImGui::Button(ui::label("Add external parent"))) {
                    std::string error;
                    const auto& parent = scene_.models()[static_cast<std::size_t>(parentIndex)];
                    if (!scene_.addExternalParent({parent.id, parentBone.data(), model->id, childBone.data()},
                                                  &error)) {
                        lastAsset_ = ui::Message("External parent: {0}", {error});
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
        if (ImGui::Button(ui::label("Copy borrowed-list"))) {
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
        if (ImGui::TreeNode(ui::label("Shortcuts"))) {
            ImGui::TextUnformatted(ui::tr("Space: Play / Pause\nCtrl+Z: Undo\nCtrl+Y: Redo\nCtrl+C/X/V: Copy/Cut/Paste "
                                          "keys\nDelete: Delete selected keys"));
            ImGui::TreePop();
        }
    }
    if (debugWorkspace)
        ImGui::End();

    if (uiState_.materialDebugVisible &&
        ImGui::Begin(workspaceWindowName("Preview Material Inspector", "material-debug").c_str())) {
        bool previewChanged = false;
        previewChanged |= ImGui::Checkbox(ui::label("Optional PMX outline"), &previewOutlineEnabled_);
        if (model != nullptr && !model->model->materials.empty()) {
            const auto materialBase = static_cast<std::int32_t>(scene_.materialBase(model->id));
            const auto localCount = static_cast<int>(model->model->materials.size());
            int localMaterial =
                previewDebugMaterial_ >= materialBase && previewDebugMaterial_ < materialBase + localCount
                    ? previewDebugMaterial_ - materialBase
                    : 0;
            const auto& selectedMaterial = model->model->materials[static_cast<std::size_t>(localMaterial)];
            if (ImGui::BeginCombo(ui::label("Material##PreviewMaterialSelector"), selectedMaterial.name.c_str())) {
                for (std::size_t index = 0; index < model->model->materials.size(); ++index) {
                    ImGui::PushID(static_cast<int>(index));
                    if (ImGui::Selectable(model->model->materials[index].name.c_str(),
                                          localMaterial == static_cast<int>(index))) {
                        localMaterial = static_cast<int>(index);
                        previewDebugMaterial_ = materialBase + localMaterial;
                        previewChanged = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if (ImGui::Button(ui::label("Show all materials"))) {
                previewDebugMaterial_ = -1;
                previewChanged = true;
            }
            const auto& material = model->model->materials[static_cast<std::size_t>(localMaterial)];
            const auto textureName = [&](std::int32_t index) {
                if (index < 0 || static_cast<std::size_t>(index) >= model->model->textures.size())
                    return std::string(ui::tr("none"));
                return mmd::pmx::resolveTexturePath(*model->model, static_cast<std::size_t>(index)).filename().string();
            };
            ImGui::Text(ui::tr("Base: %s"), textureName(material.textureIndex).c_str());
            ImGui::Text(ui::tr("Sphere: %s / mode %u"), textureName(material.sphereTextureIndex).c_str(),
                        static_cast<unsigned>(material.sphereMode));
            if (material.toonMode == 0)
                ImGui::Text(ui::tr("Toon: %s / individual"), textureName(material.toonTextureIndex).c_str());
            else
                ImGui::Text(ui::tr("Toon: shared toon%02d.bmp"), material.toonTextureIndex + 1);
            ImGui::Text(ui::tr("Diffuse alpha: %.3f  edge size: %.5f"), material.diffuse[3], material.edgeSize);
            ImGui::Text(ui::tr("Double-sided: %s  indices: %u"),
                        (material.drawFlags & 0x01U) != 0 ? ui::tr("yes") : ui::tr("no"), material.indexCount);
            previewChanged |= ImGui::CheckboxFlags(ui::label("Disable base texture"), &previewDebugFlags_,
                                                   graphics::previewDebugDisableBaseTexture);
            previewChanged |= ImGui::CheckboxFlags(ui::label("Disable sphere"), &previewDebugFlags_,
                                                   graphics::previewDebugDisableSphere);
            previewChanged |=
                ImGui::CheckboxFlags(ui::label("Disable toon"), &previewDebugFlags_, graphics::previewDebugDisableToon);
            previewChanged |=
                ImGui::CheckboxFlags(ui::label("Show normals"), &previewDebugFlags_, graphics::previewDebugNormals);
            previewChanged |= ImGui::CheckboxFlags(ui::label("Show UV"), &previewDebugFlags_, graphics::previewDebugUv);
            previewChanged |= ImGui::CheckboxFlags(ui::label("Disable sidedness"), &previewDebugFlags_,
                                                   graphics::previewDebugDisableSidedness);
        } else {
            ImGui::TextUnformatted(ui::tr("No PMX model selected"));
        }
        if (previewChanged)
            refreshPreviewScene();
    }
    if (uiState_.materialDebugVisible)
        ImGui::End();

    if (uiState_.fxDebugVisible && ImGui::Begin(workspaceWindowName("FX Debug", "fx-debug").c_str())) {
        if (const auto* effect = scene_.effect()) {
            const auto compiled = core::compileEffectGraph(*effect);
            const auto debug = core::FxRuntimeInspector::snapshot(
                *effect, static_cast<std::uint64_t>(std::max(animationFrame_, 0.0F)));
            ImGui::Text(ui::tr("Frame: %llu  Inspector: %s  Resources: %u  Global bytes: %u"),
                        static_cast<unsigned long long>(debug.frame), debug.backend.c_str(), debug.resourceCount,
                        debug.globalVarSize);
            if (!debug.memos.empty()) {
                ImGui::TextUnformatted(ui::tr("Memos:"));
                for (const auto& memo : debug.memos)
                    ImGui::BulletText("%s", memo.c_str());
            }
            ImGui::SeparatorText(ui::tr("Resources"));
            for (std::size_t index = 0; index < debug.resources.size(); ++index) {
                const auto& resource = debug.resources[index];
                ImGui::PushID(static_cast<int>(index));
                if (ImGui::TreeNode(resource.name.c_str())) {
                    ImGui::Text(ui::tr("Kind: %s  Format: %s  View: %s"), resource.kind.c_str(),
                                resource.format.c_str(), resource.view.c_str());
                    if (resource.kind == "Buffer")
                        ImGui::Text(ui::tr("Elements: %u  Element bytes: %u"), resource.width, resource.elementSize);
                    else if (resource.kind != "Sampler")
                        ImGui::Text(ui::tr("Extent: %u x %u x %u  Mips: %s"), resource.width, resource.height,
                                    resource.depth, resource.mipmapped ? ui::tr("yes") : ui::tr("no"));
                    if (!resource.sizeBase.empty())
                        ImGui::Text(ui::tr("Size base: %s"), resource.sizeBase.c_str());
                    if (!resource.shared.empty())
                        ImGui::Text(ui::tr("Shared: %s"), resource.shared.c_str());
                    if (!resource.filename.empty())
                        ImGui::TextWrapped(ui::tr("Source: %s"), resource.filename.c_str());
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (const auto& result = nativeRenderer_.debugResult(); result.has_value()) {
                ImGui::SeparatorText(result->label.c_str());
                ImGui::TextWrapped("%s", result->message.c_str());
                const auto& image = result->preview;
                if (image.width != 0 && image.height != 0) {
                    const auto columns = std::min(image.width, 128U);
                    const auto rows = std::min(image.height, 128U);
                    const float cell =
                        std::min(3.0F, std::max(1.0F, ImGui::GetContentRegionAvail().x / static_cast<float>(columns)));
                    const auto origin = ImGui::GetCursorScreenPos();
                    auto* draw = ImGui::GetWindowDrawList();
                    for (std::uint32_t y = 0; y < rows; ++y) {
                        for (std::uint32_t x = 0; x < columns; ++x) {
                            const auto pixel = (static_cast<std::size_t>(y) * image.height / rows * image.width +
                                                static_cast<std::size_t>(x) * image.width / columns) *
                                               4U;
                            draw->AddRectFilled(
                                ImVec2(origin.x + static_cast<float>(x) * cell,
                                       origin.y + static_cast<float>(y) * cell),
                                ImVec2(origin.x + static_cast<float>(x + 1) * cell,
                                       origin.y + static_cast<float>(y + 1) * cell),
                                IM_COL32(image.pixels[pixel], image.pixels[pixel + 1], image.pixels[pixel + 2], 255));
                        }
                    }
                    ImGui::Dummy(ImVec2(static_cast<float>(columns) * cell, static_cast<float>(rows) * cell));
                    ImGui::Text(ui::tr("Readback: %u x %u"), image.width, image.height);
                }
                for (std::size_t start = 0; start < std::min<std::size_t>(result->buffer.size(), 256); start += 16) {
                    std::string line;
                    for (std::size_t i = start; i < std::min(start + 16, result->buffer.size()); ++i) {
                        constexpr char hex[] = "0123456789abcdef";
                        line += hex[result->buffer[i] >> 4U];
                        line += hex[result->buffer[i] & 15U];
                        line += ' ';
                    }
                    ImGui::TextUnformatted(line.c_str());
                }
            }
            const auto liveResources = nativeRenderer_.liveResources();
            ImGui::SeparatorText(ui::tr("Live FX resource allocations"));
            ImGui::InputInt(ui::label("Mip"), &fxDebugMip_);
            ImGui::InputInt(ui::label("Depth slice"), &fxDebugSlice_);
            ImGui::Combo(ui::label("Display"), &fxDebugMode_, ui::items("RGBA on checker|RGB|R hue|G hue|B hue|A hue"));
            ImGui::DragFloat(ui::label("Display scale"), &fxDebugScale_, 0.05F);
            ImGui::InputText(ui::label("Dump path (.bin / .png / .exr)"), fxDebugDumpPath_.data(),
                             fxDebugDumpPath_.size());
            if (liveResources.empty()) {
                ImGui::TextDisabled("%s", ui::tr("No native FX resource allocations are active."));
            } else {
                for (std::size_t index = 0; index < liveResources.size(); ++index) {
                    const auto& resource = liveResources[index];
                    ImGui::PushID(static_cast<int>(index));
                    if (!resource.format.empty())
                        ImGui::Text("%s / %s  ·  %s  ·  %s", resource.effect.c_str(), resource.name.c_str(),
                                    resource.kind.c_str(), resource.format.c_str());
                    else
                        ImGui::Text("%s / %s  ·  %s", resource.effect.c_str(), resource.name.c_str(),
                                    resource.kind.c_str());
                    if (resource.kind == "Buffer") {
                        ImGui::TextDisabled(ui::tr("Type: %s  Bytes: %llu  Elements: %u  Element bytes: %u"),
                                            resource.elementType.c_str(),
                                            static_cast<unsigned long long>(resource.allocationBytes),
                                            resource.extent.width, resource.elementSize);
                    } else if (resource.kind == "Texture") {
                        ImGui::TextDisabled(ui::tr("Extent: %u x %u x %u  Dimension: %u"), resource.extent.width,
                                            resource.extent.height, resource.extent.depth, resource.dimension);
                    }
                    if (resource.kind != "Sampler") {
                        const bool preview = ImGui::Button(ui::label("Read / preview"));
                        ImGui::SameLine();
                        ImGui::BeginDisabled(fxDebugDumpPath_[0] == '\0');
                        const bool dump = ImGui::Button(ui::label("Save dump"));
                        ImGui::EndDisabled();
                        if (preview || dump) {
                            const auto hlsl = findDayoHlslDirectory(scene_.effects(), nativeRenderer_.program());
                            nativeRenderer_.requestDebugReadback(
                                {.owner = resource.owner,
                                 .name = resource.name,
                                 .generation = resource.generation,
                                 .mip = static_cast<std::uint32_t>(std::max(0, fxDebugMip_)),
                                 .slice = static_cast<std::uint32_t>(std::max(0, fxDebugSlice_)),
                                 .mode = fxDebugMode_,
                                 .scale = fxDebugScale_,
                                 .hlslDirectory = hlsl.value_or(std::filesystem::path{}),
                                 .dumpPath =
                                     dump ? std::filesystem::path(fxDebugDumpPath_.data()) : std::filesystem::path{}});
                        }
                    }
                    ImGui::PopID();
                }
            }
            ImGui::SeparatorText(ui::tr("Passes"));
            for (std::size_t index = 0; index < debug.passes.size(); ++index) {
                const auto& debugPass = debug.passes[index];
                const auto& compiledPass = compiled.passes[index];
                ImGui::PushID(static_cast<int>(index));
                if (ImGui::TreeNode(debugPass.name.c_str())) {
                    ImGui::Text(ui::tr("Type: %s  Functional: %s"), debugPass.type.c_str(),
                                debugPass.functionalKind.c_str());
                    for (const auto& access : debugPass.resources)
                        ImGui::BulletText("%s %s", access.write ? ui::tr("Write") : ui::tr("Read"),
                                          access.name.c_str());
                    for (const auto& condition : debugPass.conditions)
                        ImGui::BulletText(ui::tr("Condition: %s"), condition.c_str());
                    for (const auto& barrier : compiledPass.barriers)
                        ImGui::BulletText(ui::tr("Barrier: %s"), barrier.c_str());
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (!debug.controllers.empty()) {
                ImGui::SeparatorText(ui::tr("Controllers"));
                for (const auto& controller : debug.controllers) {
                    ImGui::Text("%s <- %s / %s (%s)", controller.name.c_str(), controller.controller.c_str(),
                                controller.item.c_str(), controller.type.c_str());
                    if (controller.slider.has_value()) {
                        const auto& slider = *controller.slider;
                        ImGui::Text(ui::tr("Range: %.4g .. %.4g  Step: %.4g  Default: %.4g%s%s"), slider.minimum,
                                    slider.maximum, slider.step, slider.defaultValue,
                                    slider.logarithmic ? ui::tr("  logarithmic") : "",
                                    slider.integer ? ui::tr("  integer") : "");
                    }
                    if (!controller.description.empty())
                        ImGui::TextWrapped("%s", controller.description.c_str());
                }
            }
        } else
            ImGui::TextUnformatted(ui::tr("Load an .fxdayo file to inspect resources and passes."));
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
        ImGui::OpenPopup(ui::label("Export Audio"));
        uiState_.audioExportOpen = false;
    }
    if (ImGui::BeginPopupModal(ui::label("Export Audio"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!DAYO_HAS_MEDIA) {
            ImGui::TextUnformatted(ui::tr("FFmpeg support is not available in this build."));
        } else {
            ImGui::TextUnformatted(ui::tr("Format"));
            ImGui::SameLine(120.0F);
            ImGui::TextUnformatted(ui::tr("M4A / AAC"));
            ImGui::TextUnformatted(ui::tr("Source"));
            ImGui::SameLine(120.0F);
            if (audioSource_.empty())
                ImGui::TextUnformatted(ui::tr("Load an audio or video asset first"));
            else
                ImGui::TextWrapped("%s", audioSource_.string().c_str());

            ImGui::SliderInt(ui::label("Bitrate (kbps)"), &audioBitrateKbps_, 64, 512);
            ImGui::TextUnformatted(ui::tr("Range"));
            ImGui::SameLine(120.0F);
            ImGui::RadioButton(ui::label("Full"), &audioRangeMode_, 0);
            ImGui::SameLine();
            ImGui::RadioButton(ui::label("Selection"), &audioRangeMode_, 1);
            if (audioRangeMode_ == 1) {
                ImGui::DragFloat(ui::label("From (seconds)"), &audioFromSeconds_, 0.1F, 0.0F, audioToSeconds_);
                ImGui::DragFloat(ui::label("To (seconds)"), &audioToSeconds_, 0.1F, audioFromSeconds_, 0.0F);
            }
            ImGui::InputText(ui::label("Output"), audioDestination_.data(), audioDestination_.size());
            ImGui::Checkbox(ui::label("Overwrite existing file"), &audioOverwrite_);
            ImGui::Separator();

            if (audioExportJob_.running()) {
                const float progress = audioExportJob_.progress();
                ImGui::ProgressBar(progress, {-1.0F, 0.0F});
                ImGui::Text("%.1f%%", static_cast<double>(progress) * 100.0);
                if (audioExportJob_.totalSeconds() > 0.0) {
                    ImGui::SameLine();
                    ImGui::Text(ui::tr("%.2f / %.2f s"), audioExportJob_.processedSeconds(),
                                audioExportJob_.totalSeconds());
                }
                if (ImGui::Button(ui::label("Cancel")))
                    audioExportJob_.cancel();
            } else {
                const auto error = audioExportJob_.error();
                if (error)
                    ImGui::TextWrapped(ui::tr("Export error: %s"), error->c_str());
                const auto result = audioExportJob_.result();
                if (result)
                    ImGui::TextWrapped(ui::tr("Exported: %s"), result->output.string().c_str());
                if (ImGui::Button(ui::label("Export"))) {
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
                        ImGui::TextWrapped(ui::tr("Export error: %s"), exception.what());
                    }
                }
            }
        }
        if (!audioExportJob_.running()) {
            ImGui::Separator();
            if (ImGui::Button(ui::label("Close")))
                ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
#endif
}

void Application::buildVideoExportUi() {
#if DAYO_HAS_IMGUI
    if (uiState_.videoExportOpen) {
        ImGui::OpenPopup(ui::label("Export Video"));
        uiState_.videoExportOpen = false;
    }
    if (ImGui::BeginPopupModal(ui::label("Export Video"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!DAYO_HAS_MEDIA) {
            ImGui::TextUnformatted(ui::tr("FFmpeg support is not available in this build."));
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
            ImGui::TextUnformatted(ui::tr("Format"));
            ImGui::SameLine(120.0F);
            ImGui::TextUnformatted(ui::tr("MP4 / H.264, H.265 or AV1"));
            ImGui::TextUnformatted(ui::tr("Source"));
            ImGui::SameLine(120.0F);
            if (audioSource_.empty())
                ImGui::TextUnformatted(ui::tr("No audio source (optional)"));
            else
                ImGui::TextWrapped("%s", audioSource_.string().c_str());
            ImGui::InputText(ui::label("Output"), videoDestination_.data(), videoDestination_.size());
            int width = static_cast<int>(videoWidth_);
            int height = static_cast<int>(videoHeight_);
            if (ImGui::InputInt(ui::label("Width"), &width))
                videoWidth_ = static_cast<std::uint32_t>(std::max(width, 1));
            if (ImGui::InputInt(ui::label("Height"), &height))
                videoHeight_ = static_cast<std::uint32_t>(std::max(height, 1));
            ImGui::InputFloat(ui::label("FPS"), &videoFps_, 0.0F, 0.0F, "%.3f");
            videoFps_ = std::max(videoFps_, 1.0F);
            ImGui::Combo(ui::label("Codec"), &videoCodec_, ui::items("H.264|H.265 / HEVC|AV1"));
            ImGui::SliderInt(ui::label("Video bitrate (kbps)"), &videoBitrateKbps_, 500, 50000);
            ImGui::SliderInt(ui::label("Audio bitrate (kbps)"), &videoAudioBitrateKbps_, 64, 512);
            ImGui::Checkbox(ui::label("Include audio"), &videoIncludeAudio_);
            auto from = static_cast<unsigned long long>(videoFromFrame_);
            auto to = static_cast<unsigned long long>(videoToFrame_);
            if (ImGui::InputScalar(ui::label("From frame"), ImGuiDataType_U64, &from))
                videoFromFrame_ = from;
            if (ImGui::InputScalar(ui::label("To frame"), ImGuiDataType_U64, &to))
                videoToFrame_ = to;
            ImGui::Checkbox(ui::label("Overwrite existing file"), &audioOverwrite_);
            ImGui::EndDisabled();
            ImGui::Separator();

            if (videoExportUiActive_) {
                if (videoExportFramesFinished_ && !videoExportJob_.running())
                    ImGui::TextUnformatted(ui::tr("Restoring preview..."));
                else if (!videoPreRollDone_)
                    ImGui::Text(ui::tr("Preparing frame %llu / %llu"),
                                static_cast<unsigned long long>(videoEvaluationNextFrame_),
                                static_cast<unsigned long long>(videoFromFrame_));
                ImGui::ProgressBar(videoExportJob_.progress(), {-1.0F, 0.0F});
                ImGui::Text("%.1f%%", static_cast<double>(videoExportJob_.progress()) * 100.0);
                if (ImGui::Button(ui::label("Cancel video export"))) {
                    videoExportJob_.requestCancel();
                    videoExportFramesFinished_ = true;
                    videoExportStatus_ = "Cancelled";
                }
            } else {
                if (!videoExportStatus_.empty())
                    ImGui::TextWrapped("%s", ui::tr(videoExportStatus_.c_str()));
                if (const auto error = videoExportJob_.error()) {
                    ImGui::TextWrapped(ui::tr("Export error: %s"), error->c_str());
                }
                if (const auto result = videoExportJob_.result()) {
                    ImGui::TextWrapped(ui::tr("Exported: %s (%.2f s)"), result->output.string().c_str(),
                                       result->durationSeconds);
                }
                if (ImGui::Button(ui::label("Export video"))) {
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
                        nativeOnStartPending_ = true;
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
            if (ImGui::Button(ui::label("Close")))
                ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
#endif
}

} // namespace dayo::app
