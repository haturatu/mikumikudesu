#include "app/application.hpp"
#include "mock_native_device.hpp"

#include <SDL3/SDL.h>
#include <mmd/pmx.hpp>
#if DAYO_HAS_IMGUI
#include <imgui.h>
#endif

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory_resource>
#include <stdexcept>

namespace {
class LargeAllocationCounter final : public std::pmr::memory_resource {
  public:
    LargeAllocationCounter() : previous_(std::pmr::set_default_resource(this)) {}
    ~LargeAllocationCounter() override {
        std::pmr::set_default_resource(previous_);
    }
    [[nodiscard]] std::size_t count() const noexcept {
        return allocations_;
    }
    void reset() noexcept {
        allocations_ = 0;
    }

  private:
    std::size_t allocations_{};
    std::pmr::memory_resource* previous_;
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        if (size >= 8U * 1024U * 1024U)
            ++allocations_;
        return previous_->allocate(size, alignment);
    }
    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        previous_->deallocate(pointer, size, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
} // namespace

namespace dayo::app {
struct ApplicationCacheTestAccess {
    static void run(Application& app, test::MockNativeDevice& device, const std::filesystem::path& modelPath,
                    LargeAllocationCounter& allocations) {
        const auto require = [](bool value, const char* message) {
            if (!value)
                throw std::runtime_error(message);
        };
        app.device_ = &device;
        device.selectRenderer(graphics::RendererKind::preview);
        core::EffectGraph effect;
        effect.category = "render";
        app.scene_.setEffect(std::move(effect));
        const auto first = app.scene_.addModel(modelPath);
        app.animationFrame_ = 1.0F;
        app.scene_.setFrame(app.animationFrame_);
        app.refreshAnimatedMesh(true, 1.0F / 60.0F);
        require(app.nativeGeometry_.empty() && app.nativeSceneModelData_.empty(),
                "Preview must not construct native geometry or model tables");
        require(app.evaluatedFrames_[0].frame.vertices.empty(), "ordinary GPU skinning must omit CPU vertex copies");
        const auto* frameBones = app.evaluatedFrames_[0].frame.bones.data();
        const auto before = app.scene_.model(first)->physics->stepStats().substeps;
        const auto body = app.scene_.model(first)->physics->bodyTransform(0);
        std::string error;
        require(app.ensureNativeSceneRuntime(false, &error), error.c_str());
        require(app.nativeSceneModelData_.size() == 1 && app.nativeSceneResources_.counts().vertexBuffers == 1,
                "native descriptors must be sized after building the current model data");
        require(app.scene_.model(first)->physics->stepStats().substeps == before &&
                    app.scene_.model(first)->physics->bodyTransform(0).position == body.position,
                "renderer initialization must reuse the pose without evaluating physics again");
        require(app.nativeSceneModelData_[0].vertices.size() == app.scene_.model(first)->model->vertices.size(),
                "native switch must reconstruct omitted bind attributes without reevaluation");
        const auto* raw = app.nativeSceneModelData_[0].rawVertices.data();
        const auto topology = app.nativeSceneModelData_[0].topologyGeneration;
        const auto material = app.nativeSceneModelData_[0].materialGeneration;
        require(app.refreshNativeSceneData() && raw == app.nativeSceneModelData_[0].rawVertices.data() &&
                    topology == app.nativeSceneModelData_[0].topologyGeneration &&
                    material == app.nativeSceneModelData_[0].materialGeneration,
                "unchanged native inputs must retain static buffers and material generations");
        app.requestRenderer(graphics::RendererKind::preview);
        const auto generation = app.nativeDeformVersion_;
        app.animationFrame_ = 2.0F;
        app.scene_.setFrame(app.animationFrame_);
        app.refreshAnimatedMesh(false, 1.0F / 30.0F);
        require(app.nativeDeformVersion_ == generation, "Preview pose updates must not rebuild native caches");
        require(app.evaluatedFrames_[0].frame.bones.data() == frameBones, "frame output must retain bone capacity");
        const auto second = app.scene_.addModel(modelPath);
        app.requestRenderer(graphics::RendererKind::subayai);
        require(app.nativePreparePending_,
                "a topology change must defer native initialization until a current pose exists");
        app.refreshAnimatedMesh(false, 1.0F / 60.0F);
        require(app.nativeSceneModelData_.size() == 2 && app.nativeSceneResources_.counts().vertexBuffers == 2,
                "adding a model during Preview must resize native descriptors before renderer preparation");
        app.requestRenderer(graphics::RendererKind::preview);
        require(app.scene_.removeModel(first), "fixture model removal failed");
        app.requestRenderer(graphics::RendererKind::subayai);
        require(app.nativePreparePending_, "removing a model must invalidate the evaluated topology");
        app.refreshAnimatedMesh(false, 1.0F / 60.0F);
        require(app.nativeGeometry_.size() == 1 && app.nativeGeometry_[0].modelId == second &&
                    app.nativeSceneResources_.counts().vertexBuffers == 1,
                "model removal must not retain the previous native model identity or descriptor count");
        app.scene_.clearModels();
        app.refreshAnimatedMesh(false);
        require(app.nativeSceneModelData_.empty() && app.evaluatedModels_.models.empty(),
                "clearing models must clear native data and controller snapshots");
        auto uvModel = mmd::pmx::load(modelPath);
        uvModel.morphs.resize(1);
        uvModel.morphs[0].name = "uv";
        uvModel.morphs[0].type = 3;
        uvModel.morphs[0].offsets = {{.index = 0, .vector4 = {0.25F, 0, 0, 0}}};
        const auto uvPath = modelPath.parent_path() / "uv.pmx";
        static_cast<void>(mmd::pmx::save(uvPath, uvModel));
        const auto uvId = app.scene_.addModel(uvPath);
        auto* instance = app.scene_.model(uvId);
        instance->motion = std::make_unique<mmd::VmdMotion>();
        instance->motion->morphs = {{"uv", 0, 0.0F}, {"uv", 30, 1.0F}};
        instance->animator->setMotion(instance->motion.get());
        app.requestRenderer(graphics::RendererKind::preview);
        app.animationFrame_ = 0;
        app.scene_.setFrame(0);
        app.refreshAnimatedMesh(true);
        require(device.previewVertices[0].uv[0] == 0.0F, "UV fixture starts with bind UVs");
        app.animationFrame_ = 30;
        app.scene_.setFrame(30);
        app.refreshAnimatedMesh(false);
        require(app.evaluatedFrames_[0].frame.vertices.size() == 3 && device.previewVertices[0].uv[0] == 0.25F,
                "GPU skinning must retain and upload changing CPU UV morphs on ordinary frames");

        app.scene_.clearModels();
        auto largeModel = mmd::pmx::load(modelPath);
        largeModel.vertices.resize(100'000, largeModel.vertices.front());
        const auto largePath = modelPath.parent_path() / "large.pmx";
        static_cast<void>(mmd::pmx::save(largePath, largeModel));
        static_cast<void>(app.scene_.addModel(largePath));
        allocations.reset();
        app.refreshAnimatedMesh(true);
        require(device.previewVertices.size() == 100'000 && allocations.count() != 0,
                "initial upload must reserve and populate the large vertex array");
        allocations.reset();
        app.refreshAnimatedMesh(false);
        require(app.evaluatedFrames_[0].frame.vertices.empty() && allocations.count() == 0,
                "unchanged GPU poses must not reserve an unused full-model vertex array");
        app.device_ = nullptr;
        app.resetProjectRuntimeState();
        app.importManagedAsset(modelPath, ui::AssetType::model);
        const auto retainedId = app.scene_.selectedModelId();
        app.device_ = &device;
        core::EffectGraph renderer;
        renderer.category = "render";
        app.scene_.setEffect(std::move(renderer));
        app.refreshAnimatedMesh(true);
        require(app.ensureNativeSceneRuntime(false, &error), error.c_str());
        const auto idleWaits = device.idleWaits;
        const auto barriers = device.retirementBarriers;
        app.removeAsset({ui::AssetType::model, retainedId});
        require(
            device.previewVertices.empty() && app.nativeGeometry_.empty() && app.nativeSceneModelData_.empty() &&
                device.idleWaits == idleWaits && device.retirementBarriers > barriers,
            "removing the last model retires native resources and unloads preview geometry without a GPU-wide wait");
        require(device.previewSettings.dof.modelIndex == previewMissingFocusModel && app.history_.undo(app.scene_) &&
                    device.previewSettings.dof.modelIndex == 0,
                "model removal clears focus ownership and Undo restores it");
        // Keep the mock alive through all application-owned GPU resource destructors.
    }
    static void runAssets(Application& app, const std::filesystem::path& modelPath) {
        using ui::AssetType;
        const auto require = [](bool value, const char* message) {
            if (!value)
                throw std::runtime_error(message);
        };
        const auto directory = modelPath.parent_path();
        app.importManagedAsset(modelPath, AssetType::model);
        const auto first = app.scene_.selectedModelId();
        const auto firstIdentity = app.scene_.model(first)->instanceId;
        const auto* parsedModel = app.scene_.model(first)->model.get();
        app.importManagedAsset(modelPath, AssetType::model);
        const auto second = app.scene_.selectedModelId();
        require(firstIdentity != app.scene_.model(second)->instanceId, "duplicate paths need independent identities");
        require(app.history_.undo(app.scene_) && app.scene_.models().size() == 1 && app.history_.redo(app.scene_) &&
                    app.scene_.model(second),
                "model addition must support Undo and Redo with stable handles");
        app.setAssetVisible({AssetType::model, first}, false);
        require(!app.scene_.model(first)->visible && app.scene_.model(first)->model.get() == parsedModel,
                "hiding a model retains parsed resources");
        app.projectEditorState_.skyboxFile = directory / "inspector.hdr";
        require(app.history_.undo(app.scene_) && app.scene_.model(first)->visible &&
                    app.projectEditorState_.skyboxFile == directory / "inspector.hdr",
                "visibility Undo must preserve unrelated Inspector settings");
        app.projectEditorState_.skyboxFile.clear();
        const auto dancePath = directory / "dance.vmd";
        const auto facePath = directory / "face.vmd";
        core::VmdMotion dance;
        dance.bones.push_back({.name = "root", .frame = 40});
        core::saveVmd(dancePath, dance);
        core::VmdMotion face;
        face.morphs.push_back({"smile", 20, 1.0F});
        core::saveVmd(facePath, face);
        app.importManagedAsset(dancePath, AssetType::motion, first);
        app.importManagedAsset(facePath, AssetType::motion, first);
        auto edited = *app.scene_.motion(first);
        edited.morphs.push_back({"edited", 30, 0.5F});
        app.history_.execute(
            app.scene_, std::make_unique<core::EditMotionCommand>(first, false, *app.scene_.motion(first), edited));
        app.removeAsset({AssetType::motion, first});
        require(!app.scene_.motion(first) && app.currentProject().assets.size() == 2,
                "clearing a merged motion removes every source reference for that model");
        require(app.history_.undo(app.scene_) && app.scene_.motion(first)->bones.size() == 1 &&
                    app.scene_.motion(first)->morphs.size() == 2,
                "motion Undo must restore both imports and timeline edits");
        require(app.history_.undo(app.scene_) && app.scene_.motion(first)->morphs.size() == 1,
                "asset Undo must remain ordered with keyframe Undo");
        require(app.history_.redo(app.scene_) && app.history_.redo(app.scene_) && !app.scene_.motion(first),
                "merged motion clear must support Redo");
        const auto posePath = directory / "pose.vpd";
        {
            std::ofstream out(posePath);
            out << "Vocaloid Pose Data file\n1.0\nmodel.pmd;\n1;\nBone0{root\n1,2,3;\n0,0,0,1;\n}\n";
        }
        app.importManagedAsset(posePath, AssetType::pose, first);
        app.removeAsset({AssetType::pose, first});
        std::filesystem::remove(posePath);
        require(!app.scene_.model(first)->pose && app.history_.undo(app.scene_) && app.scene_.model(first)->pose,
                "pose Undo must work even after its source disappears");
        // Restore the file to verify project pose ownership on reload.
        {
            std::ofstream out(posePath);
            out << "Vocaloid Pose Data file\n1.0\nmodel.pmd;\n1;\nBone0{root\n1,2,3;\n0,0,0,1;\n}\n";
        }
        const auto effectPath = directory / "model.fxdayo";
        {
            std::ofstream out(effectPath);
            out << "[YRZFX]\n{fx:{category:'deform',passes:[]}}\n[HLSL]\n";
        }
        app.importManagedAsset(effectPath, AssetType::effect, first);
        const auto effectId = app.reloadedEffects_.back().id;
        app.setAssetVisible({AssetType::effect, effectId}, false);
        require(app.scene_.effects().empty() && !app.reloadedEffects_.back().enabled,
                "disabling an effect retains its watcher without applying it");
        require(app.history_.undo(app.scene_) && app.scene_.effects().deform.size() == 1,
                "FX disable must support Undo");
        app.removeAsset({AssetType::effect, effectId});
        require(app.reloadedEffects_.empty() && app.scene_.effects().empty(),
                "effect removal stops hot reload and removes the active graph");
        const auto noFxPath = directory / "removed-fx.dayo";
        core::saveProject(noFxPath, app.currentProject());
        Application reloaded({});
        reloaded.handleAsset(noFxPath);
        require(reloaded.reloadedEffects_.empty() && reloaded.scene_.effects().empty(),
                "an explicitly removed associated FX must not be rediscovered when loading the PMX");
        require(reloaded.scene_.models().size() == 2 && reloaded.scene_.models()[0].pose &&
                    !reloaded.scene_.models()[1].pose && !reloaded.scene_.models()[0].motion,
                "saved pose ownership and empty motion snapshots must survive project reload");
        require(app.history_.undo(app.scene_) && app.reloadedEffects_.size() == 1, "effect removal must support Undo");
        core::ProjectModelState missing;
        missing.instanceId = core::newModelInstanceId();
        missing.source = directory / "missing.pmx";
        app.projectModelMetadata_.insert(app.projectModelMetadata_.begin(), missing);
        core::ProjectAsset missingAsset("pmx", missing.source);
        missingAsset.modelInstanceId = missing.instanceId;
        app.projectAssets_.insert(app.projectAssets_.begin(), missingAsset);
        core::VmdMotion missingMotion;
        missingMotion.morphs.push_back({"missing-motion", 7, 0.25F});
        app.missingModelMotions_[missing.instanceId] = missingMotion;
        require(app.scene_.addExternalParent({first, "root", second, "root"}), "parent link fixture");
        app.removeAsset({AssetType::model, first});
        const auto removed = app.currentProject();
        require(removed.models.size() == 2 && removed.models[0].instanceId == missing.instanceId &&
                    removed.models[1].instanceId == app.scene_.model(second)->instanceId &&
                    removed.embeddedMotions[1].morphs.size() == 1,
                "removal preserves missing references and aligns embedded motions with saved model order");
        require(app.reloadedEffects_.empty() && app.scene_.effects().empty() &&
                    app.scene_.timeline().externalParents.empty() && app.scene_.models().size() == 1,
                "model removal clears owned FX and external-parent links");
        const auto removedPath = directory / "removed-model.dayo";
        core::saveProject(removedPath, removed);
        const auto saved = core::loadProject(removedPath);
        require(saved.models.size() == 2 &&
                    std::ranges::none_of(saved.assets,
                                         [&](const auto& asset) { return asset.modelInstanceId == firstIdentity; }),
                "removed model and child references must not reappear after serialization");
        reloaded.handleAsset(removedPath);
        require(reloaded.scene_.models().size() == 1 && reloaded.currentProject().models.size() == 2,
                "only surviving PMX instances reload while unresolved references remain saved");
        const auto resavedPath = directory / "missing-reordered.dayo";
        core::saveProject(resavedPath, reloaded.currentProject());
        const auto resaved = core::loadProject(resavedPath);
        const auto unresolved =
            std::ranges::find(resaved.models, missing.instanceId, &core::ProjectModelState::instanceId);
        require(unresolved != resaved.models.end() &&
                    resaved.embeddedMotions[static_cast<std::size_t>(unresolved - resaved.models.begin()) + 1]
                            .morphs.size() == 1,
                "resaving unresolved models must remap motion snapshots into serialized asset order");
        require(app.history_.undo(app.scene_) && app.scene_.model(first)->model.get() == parsedModel &&
                    app.scene_.models().front().id == first && app.scene_.timeline().externalParents.size() == 1 &&
                    app.reloadedEffects_.size() == 1,
                "model Undo restores original resources, position, links, watcher and identity");
        // Validate a failed replacement before committing any mutation.
        const auto historyCount = app.history_.undoCount();
        bool failed = false;
        try {
            app.importManagedAsset(directory / "bad.pmx", AssetType::model, first, true);
        } catch (const std::exception&) {
            failed = true;
        }
        require(failed && app.history_.undoCount() == historyCount &&
                    app.scene_.model(first)->model.get() == parsedModel,
                "a failed replacement must retain the old resource and Undo history");
        app.importManagedAsset(modelPath, AssetType::model, first, true);
        require(app.scene_.model(first)->instanceId == firstIdentity && app.history_.undo(app.scene_) &&
                    app.scene_.model(first)->model.get() == parsedModel,
                "replacement retains persistent identity and Undo restores old parsed resources");
        app.duplicateModel(first);
        const auto duplicated = app.scene_.selectedModelId();
        require(duplicated != first && app.scene_.model(duplicated)->pose && app.history_.undo(app.scene_) &&
                    !app.scene_.model(duplicated) && app.history_.redo(app.scene_),
                "duplicate model copies pose and supports Undo and Redo");
        core::VmdMotion camera;
        core::VmdCameraKey cameraKey;
        cameraKey.frame = 60;
        cameraKey.distance = -20.0F;
        camera.cameras.push_back(cameraKey);
        const auto cameraPath = directory / "camera.vmd";
        core::saveVmd(cameraPath, camera);
        app.importManagedAsset(cameraPath, AssetType::globalMotion);
        app.removeAsset({AssetType::globalMotion, 0});
        require(!app.scene_.cameraMotion() && app.scene_.timeline().camera.empty() && app.history_.undo(app.scene_) &&
                    app.scene_.timeline().camera.size() == 1,
                "global motion clear and Undo rebuild camera tracks");
        const auto imagePath = directory / "background.ppm";
        {
            std::ofstream out(imagePath, std::ios::binary);
            out << "P6\n1 1\n255\n";
            out.write("\xff\0\0", 3);
        }
        app.importManagedAsset(imagePath, AssetType::image);
        app.removeAsset({AssetType::image, 0});
        require(!app.scene_.background().image && app.history_.undo(app.scene_) && app.scene_.background().image,
                "background image removal is independent and supports Undo");
#if DAYO_HAS_MEDIA
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
        require(SDL_InitSubSystem(SDL_INIT_AUDIO), "initialize dummy audio for asset lifecycle tests");
        const auto quitAudio = [](void*) { SDL_QuitSubSystem(SDL_INIT_AUDIO); };
        std::unique_ptr<void, decltype(quitAudio)> audioContext(&app, quitAudio);
        if (core::canExportVideo()) {
            const auto moviePath = directory / "movie.mp4";
            {
                core::VideoExporter exporter({.destination = moviePath,
                                              .width = 16,
                                              .height = 16,
                                              .fps = 30,
                                              .includeAudio = true,
                                              .overwrite = true});
                core::ImageRgba8 frame{16, 16, std::vector<std::uint8_t>(16 * 16 * 4, 255)};
                for (int index = 0; index < 3; ++index)
                    exporter.writeVideoFrame(frame);
                exporter.writeAudio(std::vector<float>(4800, 0.01F));
                static_cast<void>(exporter.finish());
            }
            app.importManagedAsset(moviePath, AssetType::audio);
            app.importManagedAsset(moviePath, AssetType::video);
            app.removeAsset({AssetType::audio, 0});
            require(!app.scene_.media() && app.loadedAudio_.samples.empty() && app.audioSource_.empty() &&
                        app.waveformPeaks_.empty() && app.scene_.backgroundMedia(),
                    "audio removal clears playback resources while keeping the video");
            const auto mediaPath = directory / "video-only.dayo";
            core::saveProject(mediaPath, app.currentProject());
            reloaded.handleAsset(mediaPath);
            require(reloaded.audioSource_.empty() && !reloaded.scene_.media() && reloaded.scene_.backgroundMedia(),
                    "explicitly removed movie audio must not reappear after project reload");
            require(app.history_.undo(app.scene_) && !app.loadedAudio_.samples.empty() && app.scene_.media(),
                    "audio Undo restores the decoded samples and source");
            app.removeAsset({AssetType::video, 0});
            require(!app.videoMode_ && !app.videoVisible_ && !app.scene_.backgroundMedia() &&
                        !app.scene_.background().videoPath && app.scene_.background().image && app.scene_.media(),
                    "video removal clears its decoder and flags while preserving audio and image");
            require(app.history_.undo(app.scene_) && app.scene_.backgroundMedia() && app.videoVisible_,
                    "video Undo restores the retained decoder and presentation");
        }
#endif
        app.importManagedAsset(imagePath, AssetType::environment);
        app.removeAsset({AssetType::environment, 0});
        require(app.projectEditorState_.skyboxFile.empty() && app.history_.undo(app.scene_) &&
                    app.projectEditorState_.skyboxFile == imagePath,
                "environment removal and Undo restore the environment source");
        {
            Application effectsApp({});
            const auto firstFx = directory / "renderer-a.fxdayo";
            const auto secondFx = directory / "renderer-b.fxdayo";
            for (const auto& path : {firstFx, secondFx}) {
                std::ofstream out(path);
                out << "[YRZFX]\n{fx:{category:'render',passes:[]}}\n[HLSL]\n";
            }
            effectsApp.importManagedAsset(firstFx, AssetType::effect);
            effectsApp.setAssetVisible({AssetType::effect, effectsApp.reloadedEffects_.front().id}, false);
            effectsApp.importManagedAsset(secondFx, AssetType::effect);
            std::ranges::reverse(effectsApp.projectAssets_);
            const auto effectsPath = directory / "disabled-renderer.dayo";
            core::saveProject(effectsPath, effectsApp.currentProject());
            reloaded.handleAsset(effectsPath);
            require(
                reloaded.reloadedEffects_.size() == 2 && reloaded.scene_.effects().renderer &&
                    reloaded.scene_.effects().renderer->source == secondFx &&
                    !std::ranges::find(reloaded.reloadedEffects_, firstFx, &Application::ReloadedEffect::path)->enabled,
                "a disabled renderer loaded after the active renderer must not overwrite its slot");
        }
        // A delayed callback from a previous project must be ignored.
        const auto oldGeneration = app.assetGeneration_;
        app.resetProjectRuntimeState();
        app.assetRequests_.push_back({modelPath, AssetType::model, 0, false, oldGeneration, {}});
        app.processAssetRequests();
        require(app.scene_.models().empty() && !app.history_.canUndo() && app.loadedAudio_.samples.empty(),
                "project reset clears Undo and rejects stale asynchronous imports");
    }
#if DAYO_HAS_IMGUI
    static void runSceneUi(Application& app, const std::filesystem::path& modelPath) {
        const auto require = [](bool condition, const char* message) {
            if (!condition)
                throw std::runtime_error(message);
        };
        ImGui::CreateContext();
        const auto destroyContext = [](void*) { ImGui::DestroyContext(); };
        std::unique_ptr<void, decltype(destroyContext)> context(&app, destroyContext);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = {1000, 800};
        io.DeltaTime = 1.0F / 60.0F;
        io.ConfigInputTrickleEventQueue = false;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        unsigned char* pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        app.importManagedAsset(modelPath, ui::AssetType::model);
        const auto first = app.scene_.selectedModelId();
        app.importManagedAsset(modelPath, ui::AssetType::model);
        const auto second = app.scene_.selectedModelId();
        const auto frame = [&] {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({450, 700});
            ImGui::SetNextWindowFocus();
            app.buildScenePanel();
            app.handleEditorShortcuts();
            ImGui::Render();
        };
        frame();
        frame();
        require(app.sceneDropTargets_.size() == 2 && ImGui::GetDrawData()->TotalVtxCount > 0,
                "the real Scene tree must render model rows and their drop targets");
        const auto row = app.sceneDropTargets_.front();
        io.AddMousePosEvent(row.left - 16, (row.top + row.bottom) * 0.5F);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        app.processAssetRequests();
        require(!app.scene_.model(first)->visible && app.history_.undo(app.scene_),
                "clicking the eye button must hide the corresponding model through Undo history");
        frame();
        const auto target = app.sceneDropTargets_.back();
        app.scene_.selectModel(first);
        app.queueDroppedAsset(modelPath.parent_path() / "face.vmd", (target.left + target.right) * 0.5F,
                              (target.top + target.bottom) * 0.5F);
        app.processAssetRequests();
        require(!app.scene_.model(first)->motion && app.scene_.model(second)->motion,
                "dropping VMD on a model row must target that row rather than the selected model");
        app.queueDroppedAsset(modelPath.parent_path() / "dance.vmd", -1, -1);
        require(app.assetTargetRequest_.has_value(), "dropping VMD outside model rows must ask for a target");
        app.assetTargetRequest_.reset();
        app.uiState_.selectedAsset = {ui::AssetType::model, first};
        io.AddKeyEvent(ImGuiKey_Delete, true);
        frame();
        app.processAssetRequests();
        require(!app.scene_.model(first) && app.scene_.model(second) && app.history_.undo(app.scene_),
                "Delete in the focused Scene tree must remove only the selected asset and support Undo");
        io.AddKeyEvent(ImGuiKey_Delete, false);
        frame();
        io.AddKeyEvent(ImGuiKey_Delete, true);
        ImGui::NewFrame();
        app.uiState_.sceneFocused = false;
        app.uiState_.timelineFocused = true;
        app.handleEditorShortcuts();
        ImGui::Render();
        app.processAssetRequests();
        require(app.scene_.model(first) && app.scene_.model(second), "Timeline Delete must never remove a Scene asset");
        io.AddKeyEvent(ImGuiKey_Delete, false);
        frame();
        io.AddKeyEvent(ImGuiKey_Delete, true);
        ImGui::NewFrame();
        app.uiState_.sceneFocused = true;
        io.WantTextInput = true;
        app.handleEditorShortcuts();
        ImGui::Render();
        app.processAssetRequests();
        require(app.scene_.model(first) && app.scene_.model(second),
                "Delete while entering text must never remove a Scene asset");
        app.resetProjectRuntimeState();
    }
#endif
};
} // namespace dayo::app

int main() {
    const auto directory = std::filesystem::temp_directory_path() / ("dayo-cache-" + dayo::core::newModelInstanceId());
    std::filesystem::create_directory(directory);
    try {
        mmd::PmxModel model;
        model.metadata.version = 2.0F;
        model.metadata.modelName = "native cache fixture";
        model.vertices.resize(3);
        model.vertices[1].position = {1.0F, 0.0F, 0.0F};
        model.vertices[2].position = {0.0F, 1.0F, 0.0F};
        for (auto& vertex : model.vertices) {
            vertex.normal = {0.0F, 0.0F, 1.0F};
            vertex.bones[0] = 0;
        }
        model.indices = {0, 1, 2};
        model.materials.resize(1);
        model.materials[0].diffuse = {1.0F, 1.0F, 1.0F, 1.0F};
        model.materials[0].indexCount = 3;
        model.bones.resize(1);
        model.bones[0].name = "root";
        model.rigidBodies.resize(1);
        model.rigidBodies[0].bone = 0;
        model.rigidBodies[0].mode = 1;
        model.rigidBodies[0].mass = 1.0F;
        model.rigidBodies[0].size = {0.1F, 0.1F, 0.1F};
        model.rigidBodies[0].position = {0.0F, 1.0F, 0.0F};
        const auto modelPath = directory / "model.pmx";
        static_cast<void>(mmd::pmx::save(modelPath, model));
        LargeAllocationCounter allocations;
        dayo::test::MockNativeDevice device;
        dayo::app::Application application({});
        dayo::app::ApplicationCacheTestAccess::run(application, device, modelPath, allocations);
        dayo::app::Application assetApplication({});
        dayo::app::ApplicationCacheTestAccess::runAssets(assetApplication, modelPath);
#if DAYO_HAS_IMGUI
        dayo::app::ApplicationCacheTestAccess::runSceneUi(assetApplication, modelPath);
#endif
        std::filesystem::remove_all(directory);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        std::filesystem::remove_all(directory);
        return 1;
    }
    return 0;
}
