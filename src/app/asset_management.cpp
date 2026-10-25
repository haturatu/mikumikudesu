#include "app/application.hpp"
#include "app/viewport_navigation.hpp"
#include "core/asset.hpp"
#include "core/image_hdr.hpp"
#include "core/log.hpp"
#include "core/vmdayo.hpp"

#include <SDL3/SDL.h>
#if DAYO_HAS_IMGUI
#include "ui/i18n_imgui.hpp"
#include <imgui.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

namespace dayo::app {
namespace {
using ui::AssetSelection;
using ui::AssetType;

bool globalMotion(const core::VmdMotion& motion) {
    return motion.bones.empty() && motion.morphs.empty() &&
           (!motion.cameras.empty() || !motion.lights.empty() || !motion.shadows.empty() || !motion.gravity.empty());
}

std::vector<float> waveform(const core::AudioBuffer& audio) {
    if (audio.samples.empty() || audio.channels == 0)
        return {};
    std::vector<float> peaks(1024);
    const auto frames = audio.samples.size() / audio.channels;
    for (std::size_t bucket = 0; bucket < peaks.size(); ++bucket) {
        const auto begin = bucket * frames / peaks.size();
        const auto end = std::min(frames, std::max(begin + 1, (bucket + 1) * frames / peaks.size()));
        for (auto frame = begin; frame < end; ++frame)
            for (std::uint32_t channel = 0; channel < audio.channels; ++channel)
                peaks[bucket] = std::max(peaks[bucket], std::abs(audio.samples[frame * audio.channels + channel]));
    }
    return peaks;
}
} // namespace

// Asset commands share the same history as key edits. Metadata and runtime
// ownership are restored together; Undo never reparses a removed source file.
class Application::AssetEditCommand final : public core::EditCommand {
    struct Metadata {
        std::vector<core::ProjectAsset> assets;
        std::vector<core::ProjectModelState> models;
        core::ProjectEditorState editor;
        core::SceneEffectStack effects;
        std::vector<ReloadedEffect> reloaders;
        core::BackgroundState background;
        graphics::PreviewDofSettings dof;
        std::optional<std::uint32_t> missingFocus;
        AssetSelection selection;
        core::ModelId model{};
        float frame{};
        bool videoVisible{};
    };

  public:
    AssetEditCommand(Application& app, std::string name, std::function<void()> change, std::function<void()> revert)
        : app_(app), name_(std::move(name)), change_(std::move(change)), revert_(std::move(revert)) {}
    void apply(core::Scene&) override {
        if (!initialized_) {
            before_ = capture();
            change_();
            after_ = capture();
            const auto sameWatcher = [](const ReloadedEffect& left, const ReloadedEffect& right) {
                return left.id == right.id && left.path == right.path && left.owner == right.owner &&
                       left.enabled == right.enabled && left.materialSourceFiles == right.materialSourceFiles;
            };
            const auto ids = [](const core::SceneEffectStack& stack) {
                std::vector<core::EffectId> result;
                for (const auto& effect : stack.deform)
                    result.push_back(effect.id);
                result.push_back(stack.renderer ? stack.renderer->id : 0);
                for (const auto& effect : stack.postprocess)
                    result.push_back(effect.id);
                return result;
            };
            effectsChanged_ = ids(before_.effects) != ids(after_.effects) ||
                              !std::ranges::equal(before_.reloaders, after_.reloaders, sameWatcher);
            const auto& left = before_.background;
            const auto& right = after_.background;
            backgroundChanged_ =
                left.imagePath != right.imagePath || left.videoPath != right.videoPath ||
                left.screenSource != right.screenSource || left.enabled != right.enabled || left.crop != right.crop ||
                left.mode != right.mode || left.image.has_value() != right.image.has_value() ||
                (left.image && right.image &&
                 (left.image->width != right.image->width || left.image->height != right.image->height ||
                  left.image->pixels != right.image->pixels));
            initialized_ = true;
        } else {
            change_();
            restore(after_);
        }
        app_.refreshManagedAssets();
    }
    void undo(core::Scene&) override {
        revert_();
        restore(before_);
        app_.refreshManagedAssets();
    }
    const char* name() const noexcept override {
        return name_.c_str();
    }

  private:
    Metadata capture() const {
        return {app_.projectAssets_,
                app_.projectModelMetadata_,
                app_.projectEditorState_,
                app_.scene_.effects(),
                app_.reloadedEffects_,
                app_.scene_.background(),
                app_.device_ ? app_.device_->previewRenderSettings().dof : graphics::PreviewDofSettings{},
                app_.missingFocusModelProjectIndex_,
                app_.uiState_.selectedAsset,
                app_.scene_.selectedModelId(),
                app_.scene_.timeline().frame,
                app_.videoVisible_};
    }
    void restore(const Metadata& state) {
        app_.projectAssets_ = state.assets;
        app_.projectModelMetadata_ = state.models;
        if (before_.editor.skyboxFile != after_.editor.skyboxFile)
            app_.projectEditorState_.skyboxFile = state.editor.skyboxFile;
        if (effectsChanged_) {
            app_.scene_.restoreEffects(state.effects);
            app_.reloadedEffects_ = state.reloaders;
        }
        if (backgroundChanged_)
            app_.scene_.restoreBackground(state.background);
        if (app_.device_ && before_.dof.modelIndex != after_.dof.modelIndex) {
            auto settings = app_.device_->previewRenderSettings();
            settings.dof.modelIndex = state.dof.modelIndex;
            app_.device_->setPreviewRenderSettings(settings);
        }
        if (before_.missingFocus != after_.missingFocus)
            app_.missingFocusModelProjectIndex_ = state.missingFocus;
        app_.uiState_.selectedAsset = state.selection;
        app_.scene_.selectModel(state.model);
        if (before_.frame != after_.frame)
            app_.scene_.setFrame(state.frame);
        if (before_.videoVisible != after_.videoVisible)
            app_.videoVisible_ = state.videoVisible;
    }
    Application& app_;
    std::string name_;
    std::function<void()> change_, revert_;
    Metadata before_, after_;
    bool initialized_{};
    bool effectsChanged_{}, backgroundChanged_{};
};

void Application::executeAssetEdit(std::string name, std::function<void()> change, std::function<void()> revert) {
    history_.execute(scene_,
                     std::make_unique<AssetEditCommand>(*this, std::move(name), std::move(change), std::move(revert)));
}

void Application::refreshManagedAssets() {
    // Clear every CPU cache holding ModelInstance addresses before rebuilding.
    evaluatedFrames_.clear();
    evaluatedModels_.models.clear();
    nativeGeometry_.clear();
    nativeSceneModelData_.clear();
    nativeSceneDraws_.clear();
    nativeEffectModels_.clear();
    previewFocusBindings_.clear();
    pendingFocusBindings_.clear();
    pendingFocusSnapshot_.models.clear();
    pendingTrackingFocus_ = false;
    trackingFocusFallback_ = false;
    scheduledEffects_.clear();
    nativeControllerDeclarations_.clear();
    evaluatedFramesValid_ = nativeDataValid_ = false;
    animatedTopologyGeneration_ = nativeTopologyGeneration_ = 0;
    previewFocusState_.trackingValid = false;
    previewFocusEvaluatedFrame_ = -1.0F;
    uiState_.selectedMaterial = -1;
#if DAYO_HAS_IMGUI
    timelineEditor_.clearSelection();
#endif
    animationFrame_ = scene_.timeline().frame;
    if (const auto* model = scene_.selectedModel())
        normalization_ = model->normalization;
    else
        normalization_ = {};
    videoMode_ = scene_.backgroundMedia() != nullptr;
    videoVisible_ = videoMode_ && videoVisible_;
    backgroundVideoPath_.fill(0);
    if (scene_.background().videoPath) {
        const auto text = scene_.background().videoPath->string();
        std::copy_n(text.data(), std::min(text.size(), backgroundVideoPath_.size() - 1), backgroundVideoPath_.data());
    }
    previewHdriPath_.fill(0);
    const auto skybox = projectEditorState_.skyboxFile.string();
    std::copy_n(skybox.data(), std::min(skybox.size(), previewHdriPath_.size() - 1), previewHdriPath_.data());
    failedPreviewHdriPath_.clear();
    previewHdriError_.clear();
    uploadedVideoFrame_ = -1;
    if (device_) {
        device_->cancelPreviewFocusPick();
        // Vulkan retires these resources against the submitted frame values.
        // No device-wide wait is needed when an asset leaves the scene.
        nativeSceneModelRuntime_.reset();
        nativeSceneResources_.reset();
        nativeSceneFrame_.reset();
        nativeScreenRuntime_.reset();
        if (scene_.models().empty()) {
            device_->uploadPreviewMesh({}, {});
            device_->updatePreviewMaterials({});
            device_->updatePreviewDraws({});
        }
    }
    refreshPreviewTextures();
    refreshAnimatedMesh(true);
    refreshPreviewBackground();
    refreshPreviewScene();
    requestRenderer(requestedRenderer_);
    syncMediaAtCurrentFrame();
}

void Application::removeAsset(AssetSelection asset) {
    if (asset.type == AssetType::none)
        return;
    if (asset.type == AssetType::model) {
        const auto* model = scene_.model(asset.id);
        if (!model)
            return;
        const auto identity = model->instanceId;
        auto removed = std::make_shared<std::optional<core::DetachedModel>>();
        executeAssetEdit(
            "Remove model",
            [this, asset, identity, removed] {
                const auto index = static_cast<std::uint32_t>(
                    std::ranges::find(scene_.models(), asset.id, &core::ModelInstance::id) - scene_.models().begin());
                auto metadata = currentProject().models;
                const auto saved = std::ranges::find(metadata, identity, &core::ProjectModelState::instanceId);
                const auto savedIndex = static_cast<std::size_t>(saved - metadata.begin());
                // Ownership is per instance, so another copy of the same PMX survives.
                std::erase_if(projectAssets_, [&](const auto& item) {
                    return item.modelInstanceId == identity ||
                           (item.ownerModelIndex && *item.ownerModelIndex == savedIndex);
                });
                std::erase_if(projectModelMetadata_, [&](const auto& item) { return item.instanceId == identity; });
                for (auto& item : projectAssets_) {
                    if (item.ownerModelIndex && *item.ownerModelIndex > savedIndex)
                        --*item.ownerModelIndex;
                    if (savedIndex < item.materialSourceFiles.size())
                        item.materialSourceFiles.erase(item.materialSourceFiles.begin() +
                                                       static_cast<std::ptrdiff_t>(savedIndex));
                }
                for (auto& effect : reloadedEffects_) {
                    if (effect.owner == asset.id)
                        scene_.removeEffect(effect.id);
                    if (savedIndex < effect.materialSourceFiles.size())
                        effect.materialSourceFiles.erase(effect.materialSourceFiles.begin() +
                                                         static_cast<std::ptrdiff_t>(savedIndex));
                }
                std::erase_if(reloadedEffects_, [&](const auto& effect) { return effect.owner == asset.id; });
                if (device_) {
                    auto settings = device_->previewRenderSettings();
                    if (settings.dof.modelIndex == index) {
                        settings.dof.modelIndex = previewMissingFocusModel;
                        missingFocusModelProjectIndex_.reset();
                    } else if (settings.dof.modelIndex != previewMissingFocusModel && settings.dof.modelIndex > index) {
                        --settings.dof.modelIndex;
                    }
                    if (missingFocusModelProjectIndex_ && *missingFocusModelProjectIndex_ > savedIndex)
                        --*missingFocusModelProjectIndex_;
                    device_->setPreviewRenderSettings(settings);
                }
                *removed = scene_.takeModel(asset.id);
                uiState_.selectedAsset = {};
            },
            [this, removed] {
                scene_.restoreModel(std::move(removed->value()));
                removed->reset();
            });
        return;
    }
    if (asset.type == AssetType::motion || asset.type == AssetType::globalMotion) {
        const bool global = asset.type == AssetType::globalMotion;
        if (!global && !scene_.model(asset.id))
            return;
        const auto* motion = scene_.motion(asset.id, global);
        if (!motion)
            return;
        auto saved = std::make_shared<core::VmdMotion>(*motion);
        const auto identity = global ? std::string{} : scene_.model(asset.id)->instanceId;
        executeAssetEdit(
            "Clear motion",
            [this, asset, global, identity] {
                scene_.clearMotion(asset.id, global);
                std::erase_if(projectAssets_, [&](const auto& item) {
                    return (item.kind == "vmd" || item.kind == "vmdayo") && item.modelInstanceId == identity;
                });
                uiState_.selectedAsset = {};
            },
            [this, asset, global, saved] { scene_.replaceMotion(*saved, asset.id, global); });
        return;
    }
    if (asset.type == AssetType::pose) {
        const auto* model = scene_.model(asset.id);
        if (!model || !model->pose)
            return;
        auto pose = std::make_shared<core::VpdPose>(*model->pose);
        const auto identity = model->instanceId;
        executeAssetEdit(
            "Clear pose",
            [this, asset, identity] {
                scene_.clearPose(asset.id);
                std::erase_if(projectAssets_,
                              [&](const auto& item) { return item.kind == "vpd" && item.modelInstanceId == identity; });
                uiState_.selectedAsset = {};
            },
            [this, asset, pose] { scene_.setPose(*pose, asset.id); });
        return;
    }
    if (asset.type == AssetType::effect) {
        const auto found = std::ranges::find(reloadedEffects_, asset.id, &ReloadedEffect::id);
        if (found == reloadedEffects_.end())
            return;
        const auto path = found->path;
        const auto identity = found->owner ? scene_.model(*found->owner)->instanceId : std::string{};
        executeAssetEdit(
            "Remove effect",
            [this, asset, path, identity] {
                scene_.removeEffect(asset.id);
                std::erase_if(reloadedEffects_, [&](const auto& effect) { return effect.id == asset.id; });
                std::erase_if(projectAssets_, [&](const auto& item) {
                    return item.kind == "effect" && item.path == path && item.modelInstanceId == identity;
                });
                uiState_.selectedAsset = {};
            },
            [] {});
        return;
    }
    if (asset.type == AssetType::audio) {
        struct AudioState {
            std::optional<core::MediaFile> media;
            core::AudioBuffer audio;
            std::vector<float> peaks;
            std::filesystem::path source;
            std::array<char, 1024> destination{};
            float from{}, to{};
        };
        auto state = std::make_shared<AudioState>();
        const auto swap = [this, state] {
            audioPlayer_.stop();
            scene_.swapMedia(state->media, core::MediaPresentation::audioOnly);
            std::swap(loadedAudio_, state->audio);
            waveformPeaks_.swap(state->peaks);
            audioSource_.swap(state->source);
            audioDestination_.swap(state->destination);
            std::swap(audioFromSeconds_, state->from);
            std::swap(audioToSeconds_, state->to);
        };
        executeAssetEdit(
            "Remove audio",
            [this, swap] {
                swap();
                std::erase_if(projectAssets_, [](const auto& item) { return item.kind == "audio"; });
                projectEditorState_.wavFile.clear();
                uiState_.selectedAsset = {};
            },
            swap);
        return;
    }
    if (asset.type == AssetType::video) {
        auto media = std::make_shared<std::optional<core::MediaFile>>();
        const auto swap = [this, media] { scene_.swapMedia(*media, core::MediaPresentation::backgroundVideo); };
        executeAssetEdit(
            "Remove video",
            [this, swap] {
                swap();
                scene_.clearBackgroundVideo();
                videoVisible_ = false;
                mediaVideoError_.clear();
                projectEditorState_.movieFile.clear();
                std::erase_if(projectAssets_, [](const auto& item) { return item.kind == "video"; });
                uiState_.selectedAsset = {};
            },
            swap);
        return;
    }
    executeAssetEdit(
        "Remove background",
        [this, asset] {
            if (asset.type == AssetType::image) {
                scene_.clearBackgroundImage();
                std::erase_if(projectAssets_, [](const auto& item) { return item.kind == "image"; });
            } else if (asset.type == AssetType::environment) {
                projectEditorState_.skyboxFile.clear();
            }
            uiState_.selectedAsset = {};
        },
        [] {});
}

void Application::setAssetVisible(AssetSelection asset, bool visible) {
    const auto set = [this, asset](bool value) {
        if (asset.type == AssetType::model)
            scene_.setModelVisible(asset.id, value);
        else if (asset.type == AssetType::effect) {
            const auto effect = std::ranges::find(reloadedEffects_, asset.id, &ReloadedEffect::id);
            if (effect == reloadedEffects_.end() || !effect->reloader.current())
                return;
            const auto& graph = *effect->reloader.current();
            if (value) {
                auto effects = scene_.effects();
                core::SceneEffectInstance restored{effect->id, effect->path, graph, effect->owner, 0};
                if (graph.category == "deform")
                    effects.deform.push_back(std::move(restored));
                else if (graph.category == "postprocess")
                    effects.postprocess.push_back(std::move(restored));
                else {
                    if (effects.renderer) {
                        const auto previous =
                            std::ranges::find(reloadedEffects_, effects.renderer->id, &ReloadedEffect::id);
                        if (previous != reloadedEffects_.end()) {
                            previous->enabled = false;
                            const auto owner =
                                previous->owner ? scene_.model(*previous->owner)->instanceId : std::string{};
                            for (auto& item : projectAssets_)
                                if (item.kind == "effect" && item.path == previous->path &&
                                    item.modelInstanceId == owner)
                                    item.enabled = false;
                        }
                    }
                    effects.renderer = std::move(restored);
                }
                scene_.restoreEffects(std::move(effects));
            } else
                scene_.removeEffect(effect->id);
            effect->enabled = value;
            const auto identity = effect->owner ? scene_.model(*effect->owner)->instanceId : std::string{};
            for (auto& item : projectAssets_)
                if (item.kind == "effect" && item.path == effect->path && item.modelInstanceId == identity)
                    item.enabled = value;
        } else if (asset.type == AssetType::video) {
            videoVisible_ = value;
            scene_.setBackgroundVideoVisible(value);
        } else if (asset.type == AssetType::image) {
            if (value) {
                scene_.setBackgroundEnabled(true);
                scene_.setBackgroundScreenSource(core::ScreenTextureSource::backgroundImage);
                videoVisible_ = false;
            } else if (scene_.background().screenSource == core::ScreenTextureSource::backgroundImage) {
                scene_.setBackgroundEnabled(false);
            }
        }
    };
    executeAssetEdit("Set asset visibility", [set, visible] { set(visible); }, [set, visible] { set(!visible); });
}

void Application::importManagedAsset(const std::filesystem::path& source, AssetType type, core::ModelId target,
                                     bool replace, core::EffectId replacedEffect) {
    const auto path = std::filesystem::absolute(source).lexically_normal();
    if ((type == AssetType::pose || type == AssetType::motion || (type == AssetType::effect && target != 0)) &&
        !scene_.model(target))
        throw std::runtime_error(ui::tr("Choose a target model first."));
    if (type == AssetType::model) {
        auto prepared = core::Scene::prepareModel(path);
        const auto* old = replace ? scene_.model(target) : nullptr;
        if (replace && !old)
            throw std::runtime_error(ui::tr("The target model is no longer in the scene."));
        if (old) {
            prepared.id = old->id;
            prepared.instanceId = old->instanceId;
            prepared.visible = old->visible;
            prepared.order = old->order;
            prepared.cloneCount = old->cloneCount;
            if (old->motion)
                prepared.motion = std::make_unique<core::VmdMotion>(*old->motion);
            if (old->pose)
                prepared.pose = std::make_unique<core::VpdPose>(*old->pose);
            prepared.animator->setMotion(prepared.motion.get());
            prepared.animator->setPose(prepared.pose.get());
        } else {
            const auto order = static_cast<std::int32_t>(scene_.models().size());
            prepared.order = {order, order, order, order};
        }
        std::shared_ptr<ReloadedEffect> associated;
        if (!replace) {
            if (const auto associatedPath = core::findAssociatedEffect(path)) {
                try {
                    associated = std::make_shared<ReloadedEffect>(*associatedPath, std::nullopt);
                    static_cast<void>(associated->reloader.poll());
                    if (!associated->reloader.current())
                        associated.reset();
                } catch (const std::exception& error) {
                    log::warn("Associated FX could not be loaded: ", error.what());
                }
            }
        }
        auto alternate = std::make_shared<std::optional<core::DetachedModel>>(
            core::DetachedModel{std::move(prepared), scene_.models().size(), {}});
        auto id = std::make_shared<core::ModelId>(target);
        auto retained = std::make_shared<bool>(false);
        const auto swap = [this, alternate, id, retained, replace] {
            std::optional<core::DetachedModel> removed;
            if (replace || *retained)
                removed = scene_.takeModel(*id);
            if (*alternate) {
                if (removed)
                    alternate->value().index = removed->index;
                *id = scene_.restoreModel(std::move(alternate->value()));
                *retained = true;
            } else {
                *retained = false;
            }
            *alternate = std::move(removed);
        };
        executeAssetEdit(
            replace ? "Replace model" : "Add model",
            [this, swap, id, path, associated] {
                swap();
                const auto identity = scene_.model(*id)->instanceId;
                const auto existing = std::ranges::find_if(projectAssets_, [&](const auto& item) {
                    return item.kind == "pmx" && item.modelInstanceId == identity;
                });
                if (existing != projectAssets_.end()) {
                    existing->path = path;
                } else {
                    core::ProjectAsset asset("pmx", path);
                    asset.modelInstanceId = identity;
                    projectAssets_.push_back(std::move(asset));
                }
                // Keep project order stable, including unresolved model references.
                if (std::ranges::find(projectModelMetadata_, identity, &core::ProjectModelState::instanceId) ==
                    projectModelMetadata_.end()) {
                    core::ProjectModelState state;
                    state.instanceId = identity;
                    state.source = path;
                    projectModelMetadata_.push_back(std::move(state));
                }
                if (associated) {
                    auto loaded = *associated;
                    loaded.owner = *id;
                    const auto& graph = *loaded.reloader.current();
                    if (graph.category.empty() || graph.category == "render") {
                        for (auto& previous : reloadedEffects_) {
                            if (scene_.effects().renderer && previous.id == scene_.effects().renderer->id) {
                                previous.enabled = false;
                                const auto owner =
                                    previous.owner ? scene_.model(*previous.owner)->instanceId : std::string{};
                                for (auto& item : projectAssets_)
                                    if (item.kind == "effect" && item.path == previous.path &&
                                        item.modelInstanceId == owner)
                                        item.enabled = false;
                            }
                        }
                    }
                    loaded.id = scene_.addEffect(graph, *id);
                    core::ProjectAsset effect("effect", loaded.path);
                    effect.modelInstanceId = identity;
                    projectAssets_.push_back(std::move(effect));
                    reloadedEffects_.push_back(std::move(loaded));
                }
                uiState_.selectedAsset = {AssetType::model, *id};
                uiState_.inspectScene = false;
            },
            swap);
        return;
    }
    if (type == AssetType::motion || type == AssetType::globalMotion) {
        auto motion = core::classifyAsset(path) == core::AssetKind::vmdayo
                          ? core::toVmdMotion(core::loadVmdayo(path).motion)
                          : core::loadVmd(path);
        if (type == AssetType::globalMotion && (!motion.bones.empty() || !motion.morphs.empty()))
            throw std::runtime_error(ui::tr("Choose a camera / light motion file."));
        const bool global = type == AssetType::globalMotion || globalMotion(motion);
        const auto* previous = scene_.motion(target, global);
        auto before =
            std::make_shared<std::optional<core::VmdMotion>>(previous ? std::optional{*previous} : std::nullopt);
        auto after = std::make_shared<core::VmdMotion>(std::move(motion));
        const auto identity = global ? std::string{} : scene_.model(target)->instanceId;
        executeAssetEdit(
            replace ? "Replace motion" : "Import motion",
            [this, target, global, replace, after, path, identity] {
                if (replace || global)
                    scene_.replaceMotion(*after, target, global);
                else
                    scene_.importMotion(*after, target);
                if (replace)
                    std::erase_if(projectAssets_, [&](const auto& item) {
                        return (item.kind == "vmd" || item.kind == "vmdayo") && item.modelInstanceId == identity;
                    });
                core::ProjectAsset asset(path.extension() == ".vmdayo" ? "vmdayo" : "vmd", path);
                asset.modelInstanceId = identity;
                projectAssets_.push_back(std::move(asset));
                uiState_.selectedAsset = {global ? AssetType::globalMotion : AssetType::motion, global ? 0 : target};
            },
            [this, before, target, global] {
                if (*before)
                    scene_.replaceMotion(before->value(), target, global);
                else
                    scene_.clearMotion(target, global);
            });
        return;
    }
    if (type == AssetType::pose) {
        auto pose = std::make_shared<core::VpdPose>(core::loadVpd(path));
        const auto* old = scene_.model(target)->pose.get();
        auto before = std::make_shared<std::optional<core::VpdPose>>(old ? std::optional{*old} : std::nullopt);
        const auto identity = scene_.model(target)->instanceId;
        executeAssetEdit(
            "Apply pose",
            [this, pose, target, identity, path] {
                scene_.setPose(*pose, target);
                std::erase_if(projectAssets_,
                              [&](const auto& item) { return item.kind == "vpd" && item.modelInstanceId == identity; });
                core::ProjectAsset asset("vpd", path);
                asset.modelInstanceId = identity;
                projectAssets_.push_back(std::move(asset));
                uiState_.selectedAsset = {AssetType::pose, target};
            },
            [this, before, target] { scene_.setPose(*before, target); });
        return;
    }
    if (type == AssetType::effect) {
        const auto owner = target ? std::optional{target} : std::nullopt;
        const auto existing = std::ranges::find_if(
            reloadedEffects_, [&](const auto& effect) { return effect.path == path && effect.owner == owner; });
        if (existing != reloadedEffects_.end() && !replace) {
            uiState_.selectedAsset = {AssetType::effect, existing->id};
            return;
        }
        if (existing != reloadedEffects_.end() && existing->id != replacedEffect)
            throw std::runtime_error(ui::tr("This effect is already loaded for the target."));
        // Parse first; a broken replacement must leave the active graph intact.
        auto graph = std::make_shared<core::EffectGraph>(core::loadEffectGraph(path));
        auto reloader = std::make_shared<ReloadedEffect>(path, target ? std::optional{target} : std::nullopt);
        static_cast<void>(reloader->reloader.poll());
        const auto oldId = replace ? replacedEffect : 0;
        executeAssetEdit(
            replace ? "Replace effect" : "Add effect",
            [this, graph, reloader, oldId, path, target] {
                const auto identity = target && scene_.model(target) ? scene_.model(target)->instanceId : std::string{};
                if (oldId) {
                    const auto old = std::ranges::find(reloadedEffects_, oldId, &ReloadedEffect::id);
                    if (old != reloadedEffects_.end()) {
                        const auto oldPath = old->path;
                        std::erase_if(projectAssets_, [&](const auto& item) {
                            return item.kind == "effect" && item.path == oldPath && item.modelInstanceId == identity;
                        });
                        scene_.removeEffect(oldId);
                        reloadedEffects_.erase(old);
                    }
                }
                if (graph->category.empty() || graph->category == "render") {
                    // The renderer slot has one owner; remove its previous watcher too.
                    if (scene_.effects().renderer) {
                        const auto id = scene_.effects().renderer->id;
                        const auto old = std::ranges::find(reloadedEffects_, id, &ReloadedEffect::id);
                        if (old != reloadedEffects_.end()) {
                            const auto oldPath = old->path;
                            const auto ownerId = old->owner && scene_.model(*old->owner)
                                                     ? scene_.model(*old->owner)->instanceId
                                                     : std::string{};
                            std::erase_if(projectAssets_, [&](const auto& item) {
                                return item.kind == "effect" && item.path == oldPath && item.modelInstanceId == ownerId;
                            });
                            reloadedEffects_.erase(old);
                        }
                    }
                }
                auto loaded = *reloader;
                loaded.id = scene_.addEffect(*graph, loaded.owner);
                uiState_.selectedAsset = {AssetType::effect, loaded.id};
                reloadedEffects_.push_back(std::move(loaded));
                core::ProjectAsset asset("effect", path);
                asset.modelInstanceId = identity;
                projectAssets_.push_back(std::move(asset));
            },
            [] {});
        return;
    }
    if (type == AssetType::audio) {
        struct State {
            std::optional<core::MediaFile> media;
            core::AudioBuffer audio;
            std::vector<float> peaks;
            std::filesystem::path source;
        };
        auto state = std::make_shared<State>();
        state->media.emplace(path, core::MediaOpenMode::audioOnly);
        state->audio = state->media->decodeAudio();
        state->peaks = waveform(state->audio);
        state->source = path;
        const auto swap = [this, state] {
            audioPlayer_.stop();
            scene_.swapMedia(state->media, core::MediaPresentation::audioOnly);
            std::swap(loadedAudio_, state->audio);
            waveformPeaks_.swap(state->peaks);
            audioSource_.swap(state->source);
            audioDestination_.fill(0);
            if (!audioSource_.empty())
                setAudioExportDestinationForSource(audioSource_);
            audioFromSeconds_ = 0.0F;
            audioToSeconds_ = scene_.media() ? static_cast<float>(scene_.media()->info().durationSeconds) : 0.0F;
        };
        executeAssetEdit(
            "Set audio",
            [this, swap, path] {
                swap();
                std::erase_if(projectAssets_, [](const auto& item) { return item.kind == "audio"; });
                projectAssets_.emplace_back("audio", path);
                uiState_.selectedAsset = {AssetType::audio, 0};
            },
            swap);
        return;
    }
    if (type == AssetType::video) {
        auto media = std::make_shared<std::optional<core::MediaFile>>();
        media->emplace(path, core::MediaOpenMode::videoOnly);
        const auto swap = [this, media] { scene_.swapMedia(*media, core::MediaPresentation::backgroundVideo); };
        executeAssetEdit(
            "Set video",
            [this, swap, path] {
                swap();
                scene_.setBackgroundVideo(path);
                scene_.setBackgroundEnabled(true);
                videoVisible_ = true;
                std::erase_if(projectAssets_, [](const auto& item) { return item.kind == "video"; });
                projectAssets_.emplace_back("video", path);
                uiState_.selectedAsset = {AssetType::video, 0};
            },
            swap);
        return;
    }
    if (type == AssetType::image) {
        auto image = std::make_shared<core::ImageRgba8>(core::loadImageRgba8(path));
        executeAssetEdit(
            "Set background image",
            [this, image, path] {
                auto background = scene_.background();
                background.image = *image;
                background.imagePath = path;
                background.screenSource = core::ScreenTextureSource::backgroundImage;
                background.enabled = true;
                scene_.restoreBackground(std::move(background));
                videoVisible_ = false;
                std::erase_if(projectAssets_, [](const auto& item) { return item.kind == "image"; });
                projectAssets_.emplace_back("image", path);
                uiState_.selectedAsset = {AssetType::image, 0};
            },
            [] {});
        return;
    }
    if (type == AssetType::environment) {
        // Validate before altering the environment selection.
        static_cast<void>(core::loadImageData(path));
        executeAssetEdit("Set environment", [this, path] { projectEditorState_.skyboxFile = path; }, [] {});
    }
}

void Application::duplicateModel(core::ModelId id) {
    const auto* model = scene_.model(id);
    if (!model)
        return;
    auto prepared = core::Scene::prepareModel(model->sourcePath);
    prepared.displayName = model->displayName;
    prepared.visible = model->visible;
    prepared.order = model->order;
    prepared.cloneCount = model->cloneCount;
    prepared.materialSettings = model->materialSettings;
    if (model->motion)
        prepared.motion = std::make_unique<core::VmdMotion>(*model->motion);
    if (model->pose)
        prepared.pose = std::make_unique<core::VpdPose>(*model->pose);
    prepared.animator->setMotion(prepared.motion.get());
    prepared.animator->setPose(prepared.pose.get());
    const auto originalIdentity = model->instanceId;
    auto alternate = std::make_shared<std::optional<core::DetachedModel>>(
        core::DetachedModel{std::move(prepared), scene_.models().size(), {}});
    auto newId = std::make_shared<core::ModelId>();
    const auto change = [this, alternate, newId, originalIdentity] {
        *newId = scene_.restoreModel(std::move(alternate->value()));
        alternate->reset();
        const auto* duplicate = scene_.model(*newId);
        auto sources = projectAssets_;
        for (auto& asset : sources)
            if (asset.modelInstanceId == originalIdentity && asset.kind != "effect") {
                asset.modelInstanceId = duplicate->instanceId;
                asset.ownerModelIndex.reset();
                asset.upstreamId.reset();
                projectAssets_.push_back(std::move(asset));
            }
        uiState_.selectedAsset = {AssetType::model, *newId};
    };
    executeAssetEdit("Duplicate model", change, [this, alternate, newId] { *alternate = scene_.takeModel(*newId); });
}

void Application::requestAssetDialog(AssetType type, core::ModelId target, bool replace) {
    struct Context {
        std::shared_ptr<AssetDialogQueue> queue;
        AssetRequest request;
        SDL_DialogFileFilter filter;
    };
    {
        std::lock_guard lock(assetDialogQueue_->mutex);
        if (assetDialogQueue_->pending)
            return;
        assetDialogQueue_->pending = true;
    }
    const char* pattern = "*";
    switch (type) {
    case AssetType::model:
        pattern = "pmx";
        break;
    case AssetType::motion:
    case AssetType::globalMotion:
        pattern = "vmd;vmdayo";
        break;
    case AssetType::pose:
        pattern = "vpd";
        break;
    case AssetType::effect:
        pattern = "fxdayo";
        break;
    case AssetType::image:
        pattern = "png;jpg;jpeg;bmp;tga;dds;ppm";
        break;
    case AssetType::audio:
        pattern = "wav;mp3;ogg;flac;m4a;aac;mp4;webm;mkv;avi";
        break;
    case AssetType::video:
        pattern = "mp4;webm;mkv;avi;mov";
        break;
    case AssetType::environment:
        pattern = "hdr;dds;png;jpg;jpeg;bmp";
        break;
    default:
        break;
    }
    auto* context =
        new Context{assetDialogQueue_, {{}, type, target, replace, assetGeneration_, {}}, {"Asset", pattern}};
    if (replace && type == AssetType::effect)
        context->request.replacedEffect = uiState_.selectedAsset.id;
    SDL_ShowOpenFileDialog(
        [](void* userdata, const char* const* files, int) {
            std::unique_ptr<Context> result(static_cast<Context*>(userdata));
            std::lock_guard lock(result->queue->mutex);
            result->queue->pending = false;
            if (!files) {
                result->request.error = SDL_GetError();
                result->queue->results.push_back(std::move(result->request));
            } else if (*files) {
                result->request.path = *files;
                result->queue->results.push_back(std::move(result->request));
            }
        },
        context, SDL_GetKeyboardFocus(), &context->filter, 1, nullptr, false);
}

void Application::queueDroppedAsset(const std::filesystem::path& path, float x, float y) {
    AssetType type = AssetType::none;
    switch (core::classifyAsset(path)) {
    case core::AssetKind::pmx:
        type = AssetType::model;
        break;
    case core::AssetKind::vmd:
        type = AssetType::motion;
        break;
    case core::AssetKind::vmdayo:
        type = AssetType::motion;
        break;
    case core::AssetKind::vpd:
        type = AssetType::pose;
        break;
    case core::AssetKind::effect:
        type = AssetType::effect;
        break;
    case core::AssetKind::image:
        type = AssetType::image;
        break;
    case core::AssetKind::audio:
    case core::AssetKind::video:
        type = AssetType::audio;
        break;
    default:
        break;
    }
    if (type == AssetType::none) {
        handleAsset(path);
        return;
    }
    AssetRequest request{path, type, 0, false, assetGeneration_, {}};
    if (type == AssetType::motion || type == AssetType::pose || type == AssetType::effect) {
        if (type == AssetType::motion && core::classifyAsset(path) == core::AssetKind::vmd) {
            try {
                if (globalMotion(core::loadVmd(path)))
                    request.type = AssetType::globalMotion;
            } catch (const std::exception& error) {
                lastAsset_ = ui::Message("Asset error: {0}", {error.what()});
                return;
            }
        }
        for (const auto& row : sceneDropTargets_)
            if (x >= row.left && x <= row.right && y >= row.top && y <= row.bottom)
                request.target = row.model;
        if (request.type != AssetType::globalMotion && request.target == 0) {
            if (scene_.models().size() == 1)
                request.target = scene_.models().front().id;
            else if (!scene_.models().empty()) {
                assetTargetRequest_ = std::move(request);
                return;
            }
        }
    }
    assetRequests_.push_back(std::move(request));
}

void Application::processAssetRequests() {
    if (videoExportUiActive_)
        return;
#if DAYO_HAS_IMGUI
    if (imageSequenceExportRunning_)
        return;
#endif
    {
        std::lock_guard lock(assetDialogQueue_->mutex);
        for (auto& request : assetDialogQueue_->results)
            assetRequests_.push_back(std::move(request));
        assetDialogQueue_->results.clear();
    }
    auto actions = std::move(assetActions_);
    assetActions_.clear();
    for (auto& action : actions) {
        try {
            action();
        } catch (const std::exception& error) {
            lastAsset_ = ui::Message("Asset error: {0}", {error.what()});
            log::warn(lastAsset_);
        }
    }
    if (assetRemoval_) {
        removeAsset(*assetRemoval_);
        assetRemoval_.reset();
    }
    auto requests = std::move(assetRequests_);
    assetRequests_.clear();
    for (const auto& request : requests) {
        if (request.generation != assetGeneration_)
            continue;
        try {
            if (!request.error.empty())
                throw std::runtime_error(request.error);
            if (request.path.empty())
                continue;
            importManagedAsset(request.path, request.type, request.target, request.replace, request.replacedEffect);
            lastAsset_ = ui::Message("Asset loaded: {0}", {request.path.filename().string()});
        } catch (const std::exception& error) {
            lastAsset_ = ui::Message("Asset error: {0}", {error.what()});
            log::warn(lastAsset_);
        }
    }
}

} // namespace dayo::app
