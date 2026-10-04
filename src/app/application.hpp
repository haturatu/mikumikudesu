#pragma once

#include "app/application_options.hpp"
#include "app/audio_export_job.hpp"
#include "app/video_export_job.hpp"
#include "core/editor.hpp"
#include "core/frame_scratch.hpp"
#include "core/fx/fx_controller_resolver.hpp"
#include "core/output.hpp"
#include "core/profiling.hpp"
#include "core/project.hpp"
#include "core/scene.hpp"
#include "core/video_export.hpp"
#include "editor/config.hpp"
#include "editor/editor_session.hpp"
#include "editor/interpolation_window.hpp"
#include "editor/keyframe_window.hpp"
#include "editor/pose_binding.hpp"
#include "editor/workspace.hpp"
#include "fx/fx_frame.hpp"
#include "fx/fx_scheduler.hpp"
#include "graphics/device.hpp"
#include "graphics/native_dayo_environment_runtime.hpp"
#include "graphics/native_fx_pending_events.hpp"
#include "graphics/native_oidn_provider.hpp"
#include "graphics/native_renderer.hpp"
#include "graphics/native_scene_derived_runtime.hpp"
#include "graphics/native_scene_frame_runtime.hpp"
#include "graphics/native_scene_model_runtime.hpp"
#include "graphics/native_scene_resource_store.hpp"
#include "graphics/native_screen_runtime.hpp"
#include "graphics/subayai_light_sampling.hpp"
#include "ui/ui_state.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dayo::app {

class Application { // NOLINT(clang-analyzer-optin.performance.Padding)
  public:
    explicit Application(Options options);
    int run();

  private:
    void resetProjectRuntimeState();
    [[nodiscard]] core::DayoProject currentProject() const;
    void loadEffectAsset(const std::filesystem::path& path, std::optional<core::ModelId> owner);
    void handleAsset(const std::filesystem::path& path);
    void loadAudioSource(const std::filesystem::path& path);
    void loadBackgroundVideo(const std::filesystem::path& path, bool visible = true);
    void setVideoVisible(bool visible);
    void restartAudioAtCurrentFrame();
    void seekTimeline(float frame);
    void syncMediaAtCurrentFrame();
    [[nodiscard]] double backgroundVideoSeconds() const;
    void buildMediaBackgroundUi();
    void buildMediaPlaybackControls(bool compact = false);
    void selectBackgroundSource(core::ScreenTextureSource source);
    void refreshAnimatedMesh(bool initialUpload, float deltaSeconds = 0.0F);
    void resetPhysicsSimulation();
    void evaluateExportFrame(float frame, float deltaSeconds, bool initialUpload = false);
    bool advanceDeterministicFrameEvaluation(float targetFrame, std::uint64_t& nextFrame);
    void refreshVideoFrame();
    void refreshPreviewTextures();
    void refreshPreviewBackground();
    void refreshPreviewScene();
    void buildUi();
    void buildMainMenuBar();
    void buildDockLayout();
    void buildInspectorPanel();
    void buildImageSequenceExportUi();
    void buildSaveAsDialog();
    void buildStatusBar();
    void handleEditorShortcuts();
    [[nodiscard]] std::string workspaceWindowName(const char* title, const char* id) const;
    void saveProjectNow();
    void saveProjectAsNow();
    void restoreVideoExportState();
    void startImageSequenceExport();
    void advanceImageSequenceExport();
    void finishImageSequenceExport(std::string status);
    void restoreImageSequenceState();
    void setWorkspace(ui::Workspace workspace);
    void requestRenderer(graphics::RendererKind renderer);
    [[nodiscard]] bool ensureNativeSceneRuntime(bool restartRenderer, std::string* error = nullptr);
    [[nodiscard]] fx::FxCameraState makeSceneCameraState() const;
    [[nodiscard]] fx::FxFrameContext makeNativeFrameContext(const graphics::RenderTargetDesc& target,
                                                            const fx::FxHostFrameState& invocationEvents);
    [[nodiscard]] std::optional<graphics::NativeFrameOutput>
    recordNativeFrame(graphics::CommandList& commands, const graphics::RenderTargetDesc& target);
    void setAudioExportDestinationForSource(const std::filesystem::path& source);
    void buildAudioExportUi();
    void buildVideoExportUi();
    void buildEditorUi();
    void synchronizeEditorValues();
    void buildQuitDialog();
    void updateCameraRecording();
    [[nodiscard]] core::VmdLightKey makeSceneLightState() const;
    [[nodiscard]] bool projectModified() const;
    int runVideoExport();
    [[nodiscard]] core::ModelInstance* selectedModel() noexcept {
        return scene_.selectedModel();
    }
    [[nodiscard]] const core::ModelInstance* selectedModel() const noexcept {
        return scene_.selectedModel();
    }
    [[nodiscard]] const core::FrameProfiler& frameProfiler() const noexcept {
        return frameProfiler_;
    }

    Options options_;
    graphics::Device* device_{};
    graphics::RendererKind requestedRenderer_{graphics::RendererKind::preview};
    graphics::NativeRendererCoordinator nativeRenderer_;
    std::array<char, 1024> fxDebugDumpPath_{};
    int fxDebugMip_{};
    int fxDebugSlice_{};
    int fxDebugMode_{};
    float fxDebugScale_{1.0F};
    graphics::NativeSceneFrameRuntime nativeSceneFrame_;
    graphics::NativeSceneResourceStore nativeSceneResources_;
    graphics::NativeSceneDerivedRuntime nativeSceneDerivedRuntime_;
    graphics::NativeDayoEnvironmentRuntime nativeDayoEnvironmentRuntime_;
    graphics::NativeScreenRuntime nativeScreenRuntime_;
    graphics::NativeOidnProvider nativeOidnProvider_;
    core::Scene scene_;
    core::fx::SceneEvaluationSnapshot evaluatedModels_;
    std::vector<core::EffectController> nativeControllerDeclarations_;
    core::FrameScratch frameScratch_;
    core::FrameProfiler frameProfiler_;
    core::CommandHistory history_;
    editor::EditorSession editorSession_{&scene_, &history_};
    editor::PoseBinding poseBinding_;
    editor::Workspace editorWorkspace_;
    editor::InterpolationWindow interpolationWindow_;
    editor::KeyframeWindow keyframeWindow_;
    std::optional<editor::MotionKeyId> timelineAnchor_;
    float timelineDragStartX_{};
    bool timelineBoxSelecting_{};
    std::array<float, 2> timelineBoxBegin_{};
    std::vector<editor::MotionKeyId> timelineBoxOriginal_;
    core::AudioPlayer audioPlayer_;
    AudioExportJob audioExportJob_;
    VideoExportJob videoExportJob_;
    std::vector<std::uint64_t> videoReadbacks_;
    void drainVideoReadbacks();
    std::vector<core::ImageRgba8> textures_;
    std::vector<std::uint32_t> animatedIndices_;
    std::vector<graphics::PreviewMorphDelta> animatedMorphDeltas_;
    std::vector<std::array<std::uint32_t, 2>> animatedMorphRanges_;
    std::uint64_t animatedVertexCount_{};
    std::vector<graphics::PreviewMaterial> animatedMaterialTemplates_;
    std::vector<graphics::PreviewDraw> animatedDraws_;
    std::vector<std::uint8_t> animatedEffectiveVisibility_;
    struct NativeModelGeometry {
        std::uint32_t meshId{};
        std::uint32_t cloneCount{1};
        core::ModelId modelId{};
        std::uint32_t modelIndex{};
        std::uint32_t textureBase{};
        std::uint32_t rasterizeOrder{};
        std::uint32_t deformIndex{};
        std::uint32_t deformOrder{};
        bool rasterize{};
        bool acceleration{};
        bool hasBlas{};
        std::vector<graphics::PreviewVertex> baseVertices;
        std::vector<graphics::PreviewBoneTransform> bones;
        std::vector<graphics::PreviewMorphDelta> morphDeltas;
        std::vector<float> morphWeights;
        std::vector<std::uint32_t> indices;
        std::vector<graphics::NativeDeformedVertex> deformedVertices;
    };
    std::vector<NativeModelGeometry> nativeGeometry_;
    std::vector<graphics::NativeSceneDraw> nativeSceneDraws_;
    std::vector<graphics::NativeEffectModel> nativeEffectModels_;
    graphics::NativeSceneModelRuntime nativeSceneModelRuntime_;
    std::vector<graphics::NativeSceneModelData> nativeSceneModelData_;
    graphics::LightSamplingService nativeLightSampling_;
    std::vector<float> nativeLightPowers_;
    std::uint64_t nativeDeformVersion_{};
    std::uint64_t nativeMaterialGeneration_{1};
    graphics::NativeFxPendingEvents nativeFxPendingEvents_;
    std::uint64_t animatedTopologyGeneration_{};
    float animationFrame_{};
    int uploadedAnimationFrame_{-1};
    bool playing_{true};
    bool nativePlaybackWasActive_{};
    bool nativeOnStartPending_{true};
    bool repeat_{true};
    float playbackSpeed_{1.0F};
    float audioVolume_{1.0F};
    float audioOffsetSeconds_{};
    core::AudioBuffer loadedAudio_;
    std::vector<float> waveformPeaks_;
    bool videoMode_{}; // A background video is loaded.
    std::string mediaVideoError_;
    bool videoVisible_{}; // Decode and display only when explicitly enabled.
    std::array<char, 1024> backgroundVideoPath_{};
    double mediaSeconds_{};
    std::int64_t uploadedVideoFrame_{-1};
    std::string lastAsset_{"Drop PMX/VMD/VPD/media files into the window"};
    std::vector<core::ProjectAsset> projectAssets_;
    std::vector<std::pair<std::filesystem::path, core::ModelId>> modelAssetOwners_;
    std::vector<core::ProjectModelState> projectModelMetadata_;
    core::ProjectEditorState projectEditorState_;
    core::ProjectEditorState savedEditorState_;
    std::uint64_t savedHistoryRevision_{};
    std::size_t savedAssetCount_{};
    bool unsavedModelChanges_{};
    bool quitRequested_{};
    bool quitConfirmed_{};
    bool overwriteApproved_{};
    std::array<char, 1024> previewHdriPath_{};
    std::filesystem::path failedPreviewHdriPath_;
    std::uint64_t failedPreviewHdriVersion_{};
    std::string previewHdriError_;
    std::string upstreamDocumentJson_;
    std::optional<std::filesystem::path> currentProjectPath_;
    struct ReloadedEffect {
        std::filesystem::path path;
        core::EffectId id{};
        std::optional<core::ModelId> owner;
        std::vector<std::vector<std::string>> materialSourceFiles;
        core::EffectHotReloader reloader;

        ReloadedEffect(std::filesystem::path source, std::optional<core::ModelId> model)
            : path(std::move(source)), owner(model), reloader(path) {}
    };
    std::vector<ReloadedEffect> reloadedEffects_;
    fx::FrameEffectScheduler effectScheduler_;
    std::vector<fx::ScheduledFx> scheduledEffects_;
    core::PreviewNormalization normalization_;
    core::Float3 cameraPan_{};
    int previewResolutionDivisor_{1};
    editor::EditorConfig editorConfig_;
    bool preferencesVisible_{};
    bool borrowedAssetsVisible_{};
    bool shortcutsVisible_{};
    bool historyVisible_{};
    bool modelToolsVisible_{};
    core::ModelId annotationModel_{};
    int annotationMaterial_{-1};
    std::array<char, 1024> materialAnnotation_{};
    float cameraYaw_{};
    float cameraPitch_{};
    float cameraDistance_{3.0F};
    bool manualCamera_{};
    std::int32_t previewDebugMaterial_{-1};
    std::uint32_t previewDebugFlags_{};
    bool previewOutlineEnabled_{};
    std::filesystem::path audioSource_;
    std::array<char, 1024> audioDestination_{};
#if DAYO_HAS_IMGUI
    int audioBitrateKbps_{192};
    int audioRangeMode_{};
    bool audioOverwrite_{};
    std::array<char, 1024> videoDestination_{};
    std::uint32_t videoWidth_{1920};
    std::uint32_t videoHeight_{1080};
    std::uint32_t sequenceWidth_{1920};
    std::uint32_t sequenceHeight_{1080};
    int previewStillScale_{1}; // 0: native, 1: 4/3, 2: 2x
    int sequencePreset_{2};
#endif
    float videoFps_{30.0F};
#if DAYO_HAS_IMGUI
    int videoCodec_{};
    int videoBitrateKbps_{8000};
    int videoAudioBitrateKbps_{192};
    bool videoIncludeAudio_{true};
#endif
    float audioFromSeconds_{};
    float audioToSeconds_{};
    std::uint64_t videoFromFrame_{};
    std::uint64_t videoToFrame_{};
    std::uint64_t videoNextFrame_{};
    std::uint64_t videoOutputFrameCount_{};
    double videoSourceFps_{30.0};
    float videoPreviousSourceFrame_{};
    bool videoPreRollDone_{};
    bool videoExportFramesFinished_{};
    bool videoExportUiActive_{};
    struct ActiveVideoExport {
        std::uint32_t width{};
        std::uint32_t height{};
    };
    std::optional<ActiveVideoExport> activeVideoExport_;
    bool videoExportRestorePending_{};
    std::uint64_t videoEvaluationNextFrame_{};
    std::uint64_t videoRestoreNextFrame_{};
    float videoExportRestoreFrame_{};
    double videoExportRestoreMediaSeconds_{};
    bool videoExportRestorePlaying_{};
    bool videoExportRestoreManualCamera_{};
    bool videoExportRestoreAudioActive_{};
    bool videoRangeInitialized_{};
    std::string videoExportStatus_;
    ui::UiState uiState_;
    core::MotionClipboard motionClipboard_;
    float timelineZoom_{1.0F};
    float timelinePan_{};
    float timelineScrollY_{};
    bool editGlobalMotion_{};
    bool recordCamera_{};
    int selectedMorph_{};
    core::ModelId editorValuesModel_{};
    float editorValuesFrame_{-1};
    std::uint64_t editorValuesRevision_{};
    core::ModelId morphEditModel_{};
    int morphEditIndex_{-1};
    bool morphModified_{};
    std::unique_ptr<editor::UndoTransaction> cameraRecordingTransaction_;
    std::int64_t recordedCameraFrame_{-1};
    std::int64_t cameraRecordingEnd_{};
    core::RuntimeMode cameraRecordingMode_{core::RuntimeMode::realtime};
    bool cameraRecordedAny_{};
    bool cameraModified_{};
    bool lightModified_{};
    bool shadowModified_{};
    float baseMorphWeight_{};
    core::VmdCameraKey baseCamera_;
    core::VmdLightKey baseLight_;
    core::VmdShadowKey baseShadow_;
    bool physicsDebug_{};
    float editedMorphWeight_{};
    core::VmdCameraKey editedCamera_;
    std::array<char, 256> cameraParentBoneName_{};
    core::VmdLightKey editedLight_{0, {0.6F, 0.6F, 0.6F}, {-0.5F, -1.0F, 0.5F}};
    core::VmdShadowKey editedShadow_{0, 1, 50.0F};
    core::OutputSettings sequenceOutput_;
    std::string sequenceOutputStatus_;
    std::array<char, 1024> projectDestination_{'p', 'r', 'o', 'j', 'e', 'c', 't', '.', 'd', 'a', 'y', 'o', '\0'};
    std::string projectSaveStatus_;
    std::array<char, 1024> sequenceOutputDirectory_{'o', 'u', 't', 'p', 'u', 't', '\0'};
    std::array<char, 1024> sequenceOutputFilename_{'f', 'r', 'a', 'm', 'e', '_', '0', '0',
                                                   '0', '0', '0', '.', 'p', 'p', 'm', '\0'};
    struct TimelineTrack {
        std::string name;
        std::vector<std::uint32_t> frames;
    };
    struct TimelineTrackCache {
        std::uint64_t motionRevision{};
        core::ModelId modelId{};
        const core::VmdMotion* motion{};
        bool globalMotion{};
        std::vector<TimelineTrack> bones;
        std::vector<TimelineTrack> morphs;
        std::vector<std::uint32_t> cameras;
        std::vector<std::uint32_t> lights;
        std::vector<std::uint32_t> shadows;
        std::vector<std::uint32_t> ik;
    };
    TimelineTrackCache timelineTrackCache_;
    bool timelineKeyListVisible_{};
    bool imageSequenceExportRunning_{};
    bool imageSequenceCancelRequested_{};
    bool imageSequenceFramesFinished_{};
    bool imageSequencePreRollDone_{};
    bool imageSequenceRestoring_{};
    std::optional<core::OutputQueue> imageSequenceOutput_;
    std::uint32_t imageSequenceNextFrame_{};
    std::uint32_t imageSequenceSampleIndex_{};
    std::uint32_t imageSequenceSampleCount_{1};
    std::uint64_t imageSequencePreRollFrame_{};
    std::uint64_t imageSequenceRestoreNextFrame_{};
    float imageSequencePreviousSampleFrame_{};
    float imageSequenceRestoreFrame_{};
    double imageSequenceRestoreMediaSeconds_{};
    bool imageSequenceRestorePlaying_{};
    bool imageSequenceRestoreManualCamera_{};
    core::ImageRgba8 imageSequenceImage_;
    graphics::Rgba16fSampleAccumulator imageSequenceHdrSamples_;
    std::string imageSequenceCompletionStatus_;
};

} // namespace dayo::app
