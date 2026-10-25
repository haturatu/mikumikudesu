#include "app/application.hpp"
#include "app/viewport_navigation.hpp"

#include <SDL3/SDL.h>
#if DAYO_HAS_IMGUI
#include "ui/i18n_imgui.hpp"
#include <imgui.h>
#endif

#include <algorithm>
#include <cctype>

namespace dayo::app {
using ui::AssetSelection;
using ui::AssetType;

void Application::buildScenePanel() {
#if DAYO_HAS_IMGUI
    uiState_.sceneFocused = false;
    sceneDropTargets_.clear();
    if (!uiState_.sceneVisible)
        return;
    if (!ImGui::Begin(workspaceWindowName("Scene", "scene").c_str())) {
        ImGui::End();
        return;
    }
    uiState_.sceneFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const auto select = [this](AssetSelection asset) {
        uiState_.selectedAsset = asset;
        uiState_.inspectScene =
            asset.type != AssetType::model && asset.type != AssetType::motion && asset.type != AssetType::pose;
        if (!uiState_.inspectScene && scene_.model(asset.id)) {
            scene_.selectModel(asset.id);
            normalization_ = scene_.model(asset.id)->normalization;
        }
        inspectorPage_ = asset.type == AssetType::effect ? InspectorPage::effects : InspectorPage::selection;
    };
    bool dialogPending{};
    {
        std::lock_guard lock(assetDialogQueue_->mutex);
        dialogPending = assetDialogQueue_->pending;
    }
    ImGui::BeginDisabled(dialogPending || videoExportUiActive_ || imageSequenceExportRunning_);
    if (ImGui::Button(ui::label("+ Add…")))
        ImGui::OpenPopup("add-scene-asset");
    if (ImGui::BeginPopup("add-scene-asset")) {
        const auto target = scene_.selectedModelId();
        const auto add = [&](const char* label, AssetType type, core::ModelId model = 0, bool enabled = true) {
            if (ImGui::MenuItem(ui::label(label), nullptr, false, enabled))
                requestAssetDialog(type, model);
        };
        add("Model…", AssetType::model);
        add("Motion…", AssetType::motion, target, target != 0);
        add("Pose…", AssetType::pose, target, target != 0);
        add("Camera / light motion…", AssetType::globalMotion);
        add("Effect…", AssetType::effect, target);
        ImGui::Separator();
        add("Audio…", AssetType::audio);
        add("Video…", AssetType::video);
        add("Background image…", AssetType::image);
        add("Environment…", AssetType::environment);
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
    if (ImGui::Selectable(ui::label("Scene / Background"),
                          uiState_.inspectScene && uiState_.selectedAsset.type == AssetType::none)) {
        uiState_.selectedAsset = {};
        uiState_.inspectScene = true;
        inspectorPage_ = InspectorPage::selection;
    }
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputTextWithHint("##scene-filter", ui::tr("Search scene..."), uiState_.sceneFilter.data(),
                             uiState_.sceneFilter.size());
    const auto matches = [this](std::string value) {
        std::string filter = uiState_.sceneFilter.data();
        const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
        std::ranges::transform(value, value.begin(), lower);
        std::ranges::transform(filter, filter.begin(), lower);
        return value.find(filter) != std::string::npos;
    };
    const auto sourceNames = [this](const std::string& identity, const char* kind) {
        std::string names;
        for (const auto& asset : projectAssets_)
            if (asset.modelInstanceId == identity &&
                (asset.kind == kind || (std::string_view(kind) == "vmd" && asset.kind == "vmdayo"))) {
                if (!names.empty())
                    names += ", ";
                names += asset.path.filename().string();
            }
        return names;
    };
    const auto context = [&](AssetSelection asset, const std::filesystem::path& source, core::ModelId target = 0) {
        if (!ImGui::BeginPopupContextItem("asset-context"))
            return;
        select(asset);
        if (asset.type == AssetType::model) {
            const auto* model = scene_.model(asset.id);
            if (ImGui::MenuItem(ui::label(model->visible ? "Hide" : "Show")))
                assetActions_.emplace_back(
                    [this, asset, visible = !model->visible] { setAssetVisible(asset, visible); });
            if (ImGui::MenuItem(ui::label("Focus model"))) {
                ViewportNavigation::reset(cameraTarget_, cameraYaw_, cameraPitch_, cameraDistance_,
                                          model->normalization.center, model->normalization.scale);
                manualCamera_ = true;
                cameraEditPreview_ = false;
                followVmdCamera_ = false;
                refreshPreviewScene();
            }
            if (ImGui::MenuItem(ui::label("Rename…"))) {
                renameModel_ = asset.id;
                modelName_.fill(0);
                std::copy_n(model->displayName.data(), std::min(model->displayName.size(), modelName_.size() - 1),
                            modelName_.data());
            }
            if (ImGui::MenuItem(ui::label("Duplicate model")))
                assetActions_.emplace_back([this, asset] { duplicateModel(asset.id); });
        }
        if (asset.type == AssetType::effect) {
            const auto effect = std::ranges::find(reloadedEffects_, asset.id, &ReloadedEffect::id);
            if (effect != reloadedEffects_.end() && ImGui::MenuItem(ui::label(effect->enabled ? "Disable" : "Enable")))
                assetActions_.emplace_back(
                    [this, asset, enabled = effect->enabled] { setAssetVisible(asset, !enabled); });
        }
        if (ImGui::MenuItem(ui::label("Replace file…")))
            requestAssetDialog(asset.type, asset.type == AssetType::model ? asset.id : target, true);
        if (!source.empty() && ImGui::MenuItem(ui::label("Open Source Folder"))) {
            const auto folder = source.parent_path().generic_string();
            static constexpr char hex[] = "0123456789ABCDEF";
            std::string url = "file://";
            for (const char character : folder) {
                const auto c = static_cast<unsigned char>(character);
                if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
                    url += static_cast<char>(c);
                else {
                    url += '%';
                    url += hex[c >> 4U];
                    url += hex[c & 15U];
                }
            }
            if (!SDL_OpenURL(url.c_str()))
                lastAsset_ = ui::Message("Open folder: {0}", {SDL_GetError()});
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ui::label(asset.type == AssetType::motion || asset.type == AssetType::globalMotion
                                          ? "Clear all motion"
                                          : "Remove from scene"),
                            "Delete"))
            assetRemoval_ = asset;
        ImGui::EndPopup();
    };
    const auto visibility = [&](AssetSelection asset, bool visible) {
        if (ImGui::Button("##visibility", {24.0F, ImGui::GetFrameHeight()}))
            assetActions_.emplace_back([this, asset, visible] { setAssetVisible(asset, !visible); });
        const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        const ImVec2 centre{(min.x + max.x) * 0.5F, (min.y + max.y) * 0.5F};
        const auto color = ImGui::GetColorU32(ImGuiCol_Text);
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddEllipse(centre, {8.0F, 4.0F}, color);
        draw->AddCircleFilled(centre, 2.0F, color);
        if (!visible)
            draw->AddLine({centre.x - 8, centre.y + 6}, {centre.x + 8, centre.y - 6}, color, 1.5F);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", ui::tr(visible ? "Hide" : "Show"));
        ImGui::SameLine();
    };
    const auto row = [&](AssetSelection asset, const std::string& label, const std::filesystem::path& source = {},
                         core::ModelId target = 0) {
        ImGui::PushID(static_cast<int>(asset.type));
        ImGui::PushID(static_cast<int>(asset.id));
        if (asset.type == AssetType::effect) {
            const auto effect = std::ranges::find(reloadedEffects_, asset.id, &ReloadedEffect::id);
            if (effect != reloadedEffects_.end())
                visibility(asset, effect->enabled);
        }
        if (asset.type == AssetType::video)
            visibility(asset, videoVisible_);
        if (asset.type == AssetType::image)
            visibility(asset, scene_.background().enabled &&
                                  scene_.background().screenSource == core::ScreenTextureSource::backgroundImage);
        auto flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (uiState_.selectedAsset == asset)
            flags |= ImGuiTreeNodeFlags_Selected;
        ImGui::TreeNodeEx("asset", flags, "%s", label.c_str());
        if (ImGui::IsItemClicked())
            select(asset);
        if (ImGui::IsItemHovered()) {
            if (asset.type == AssetType::motion || asset.type == AssetType::globalMotion)
                ImGui::SetTooltip("%s", ui::tr("Imported motion files are merged. Clear removes all keys, including "
                                               "timeline edits. Undo restores them."));
            else if (!source.empty())
                ImGui::SetTooltip("%s", source.string().c_str());
        }
        context(asset, source, target);
        ImGui::PopID();
        ImGui::PopID();
    };
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    const auto modelTitle = std::string(ui::tr("Models")) + " (" + std::to_string(scene_.models().size()) + ")";
    if (ImGui::TreeNodeEx("models", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", modelTitle.c_str())) {
        if (scene_.models().empty())
            ImGui::TextDisabled("%s", ui::tr("Add a model with + Add or drop a PMX file."));
        for (const auto& model : scene_.models()) {
            const auto motionNames = sourceNames(model.instanceId, "vmd");
            const auto poseNames = sourceNames(model.instanceId, "vpd");
            std::string searchable = model.displayName;
            searchable += model.sourcePath.string();
            searchable += motionNames;
            searchable += poseNames;
            for (const auto& effect : reloadedEffects_)
                if (effect.owner == model.id)
                    searchable += effect.path.string();
            if (!matches(searchable))
                continue;
            ImGui::PushID(static_cast<int>(model.id));
            visibility({AssetType::model, model.id}, model.visible);
            auto flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (uiState_.selectedAsset == AssetSelection{AssetType::model, model.id})
                flags |= ImGuiTreeNodeFlags_Selected;
            ImGui::SetNextItemOpen(true, uiState_.sceneFilter[0] ? ImGuiCond_Always : ImGuiCond_Once);
            const bool open = ImGui::TreeNodeEx("model", flags, "%s", model.displayName.c_str());
            const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            sceneDropTargets_.push_back({model.id, min.x, min.y, max.x, max.y});
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                select({AssetType::model, model.id});
            context({AssetType::model, model.id}, model.sourcePath, model.id);
            if (open) {
                if (model.motion)
                    row({AssetType::motion, model.id},
                        std::string(ui::tr("Motion (merged)")) + (motionNames.empty() ? "" : ": " + motionNames), {},
                        model.id);
                if (model.pose)
                    row({AssetType::pose, model.id},
                        std::string(ui::tr("Pose")) + (poseNames.empty() ? "" : ": " + poseNames), {}, model.id);
                for (const auto& effect : reloadedEffects_)
                    if (effect.owner == model.id)
                        row({AssetType::effect, effect.id}, effect.path.filename().string(), effect.path, model.id);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNodeEx(ui::label("Camera / Light"), ImGuiTreeNodeFlags_SpanAvailWidth)) {
        if (scene_.cameraMotion() && matches("Camera / Light" + sourceNames({}, "vmd")))
            row({AssetType::globalMotion, 0}, std::string(ui::tr("Motion (merged)")) + ": " + sourceNames({}, "vmd"));
        ImGui::TreePop();
    }
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNodeEx(ui::label("Effects"), ImGuiTreeNodeFlags_SpanAvailWidth)) {
        for (const auto& effect : reloadedEffects_)
            if (!effect.owner && matches(effect.path.string()))
                row({AssetType::effect, effect.id}, effect.path.filename().string(), effect.path);
        ImGui::TreePop();
    }
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNodeEx(ui::label("Media"), ImGuiTreeNodeFlags_SpanAvailWidth)) {
        if (!audioSource_.empty() && matches(audioSource_.string()))
            row({AssetType::audio, 0}, audioSource_.filename().string() + " (" + ui::tr("Audio") + ")", audioSource_);
        if (scene_.background().videoPath && matches(scene_.background().videoPath->string()))
            row({AssetType::video, 0},
                scene_.background().videoPath->filename().string() + " (" + ui::tr("Video") + ")",
                *scene_.background().videoPath);
        ImGui::TreePop();
    }
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNodeEx(ui::label("Background / Environment"), ImGuiTreeNodeFlags_SpanAvailWidth)) {
        if (scene_.background().imagePath && matches(scene_.background().imagePath->string()))
            row({AssetType::image, 0}, scene_.background().imagePath->filename().string(),
                *scene_.background().imagePath);
        if (!projectEditorState_.skyboxFile.empty() && matches(projectEditorState_.skyboxFile.string()))
            row({AssetType::environment, 0}, projectEditorState_.skyboxFile.filename().string(),
                projectEditorState_.skyboxFile);
        ImGui::TreePop();
    }
    if (renameModel_ && !ImGui::IsPopupOpen("rename-model"))
        ImGui::OpenPopup("rename-model");
    if (ImGui::BeginPopupModal("rename-model", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool enter = ImGui::InputText(ui::label("Name"), modelName_.data(), modelName_.size(),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button(ui::label("Apply")) || enter) && modelName_[0]) {
            const auto id = renameModel_;
            const auto name = std::string(modelName_.data());
            if (const auto* model = scene_.model(id)) {
                const auto before = model->displayName;
                assetActions_.emplace_back([this, id, before, name] {
                    executeAssetEdit(
                        "Rename model",
                        [this, id, name] {
                            if (auto* renamed = scene_.model(id))
                                renamed->displayName = name;
                        },
                        [this, id, before] {
                            if (auto* renamed = scene_.model(id))
                                renamed->displayName = before;
                        });
                });
            }
            renameModel_ = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(ui::label("Cancel"))) {
            renameModel_ = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (assetTargetRequest_ && !ImGui::IsPopupOpen("asset-target"))
        ImGui::OpenPopup("asset-target");
    if (ImGui::BeginPopupModal("asset-target", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(ui::tr("Choose target model"));
        if (assetTargetRequest_) {
            ImGui::TextUnformatted(assetTargetRequest_->path.filename().string().c_str());
            for (const auto& model : scene_.models()) {
                ImGui::PushID(static_cast<int>(model.id));
                if (ImGui::Selectable(model.displayName.c_str())) {
                    assetTargetRequest_->target = model.id;
                    assetRequests_.push_back(*assetTargetRequest_);
                    assetTargetRequest_.reset();
                    ImGui::CloseCurrentPopup();
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
        }
        if (ImGui::Button(ui::label("Cancel"))) {
            assetTargetRequest_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
#endif
}
} // namespace dayo::app
