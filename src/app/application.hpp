#pragma once

#include "app/application_options.hpp"
#include "app/audio_export_job.hpp"
#include "app/video_export_job.hpp"
#include "core/editor.hpp"
#include "core/frame_scratch.hpp"
#include "core/output.hpp"
#include "core/profiling.hpp"
#include "core/project.hpp"
#include "core/scene.hpp"
#include "core/task_scheduler.hpp"
#include "core/video_export.hpp"
#include "graphics/device.hpp"
#include "ui/ui_state.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dayo::app {

class Application { // NOLINT(clang-analyzer-optin.performance.Padding)
  public:
    explicit Application(Options options);
    int run();

  private:
    void resetProjectRuntimeState();
    [[nodiscard]] core::DayoProject currentProject() const;
    void handleAsset(const std::filesystem::path& path);
    void refreshAnimatedMesh(bool initialUpload, float deltaSeconds = 0.0F);
    void resetPhysicsSimulation();
    void evaluateExportFrame(float frame, float deltaSeconds, bool initialUpload = false);
    void prepareDeterministicFrameEvaluation(float targetFrame);
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
    void startImageSequenceExport();
    void advanceImageSequenceExport();
    void finishImageSequenceExport(std::string status);
    void restoreImageSequenceState();
    void setWorkspace(ui::Workspace workspace);
    void setAudioExportDestinationForSource(const std::filesystem::path& source);
    void buildAudioExportUi();
    void buildVideoExportUi();
    void buildEditorUi();
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
    core::Scene scene_;
    core::TaskScheduler taskScheduler_;
    core::FrameScratch frameScratch_;
    core::FrameProfiler frameProfiler_;
    core::CommandHistory history_;
    core::AudioPlayer audioPlayer_;
    AudioExportJob audioExportJob_;
    VideoExportJob videoExportJob_;
    std::vector<core::ImageRgba8> textures_;
    std::vector<std::uint32_t> animatedIndices_;
    std::vector<graphics::PreviewMorphDelta> animatedMorphDeltas_;
    std::vector<std::array<std::uint32_t, 2>> animatedMorphRanges_;
    std::uint64_t animatedVertexCount_{};
    std::vector<graphics::PreviewMaterial> animatedMaterialTemplates_;
    std::vector<graphics::PreviewDraw> animatedDraws_;
    std::uint64_t animatedTopologyGeneration_{};
    float animationFrame_{};
    int uploadedAnimationFrame_{-1};
    bool playing_{true};
    bool repeat_{true};
    float playbackSpeed_{1.0F};
    float audioVolume_{1.0F};
    float audioOffsetSeconds_{};
    core::AudioBuffer loadedAudio_;
    std::vector<float> waveformPeaks_;
    bool videoMode_{};
    double mediaSeconds_{};
    std::int64_t uploadedVideoFrame_{-1};
    std::string lastAsset_{"Drop PMX/VMD/VPD/media files into the window"};
    std::vector<core::ProjectAsset> projectAssets_;
    std::optional<core::EffectHotReloader> effectReloader_;
    core::PreviewNormalization normalization_;
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
    bool videoRangeInitialized_{};
    std::string videoExportStatus_;
#if DAYO_HAS_IMGUI
    ui::UiState uiState_;
    core::MotionClipboard motionClipboard_;
    std::vector<core::MotionKeyRef> selectedKeys_;
    float timelineZoom_{1.0F};
    float timelinePan_{};
    float timelineScrollY_{};
    bool editGlobalMotion_{};
    bool recordCamera_{};
    int selectedBone_{};
    int selectedMorph_{};
    core::Float3 editedBoneTranslation_{};
    core::Float4 editedBoneRotation_{0.0F, 0.0F, 0.0F, 1.0F};
    bool editedBonePhysics_{true};
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
    struct TimelineTrack {
        std::string name;
        std::vector<std::uint32_t> frames;
    };
    struct TimelineTrackCache {
        core::ModelId modelId{};
        const core::VmdMotion* motion{};
        bool globalMotion{};
        std::vector<TimelineTrack> bones;
        std::vector<TimelineTrack> morphs;
    };
    TimelineTrackCache timelineTrackCache_;
    bool imageSequenceExportRunning_{};
    bool imageSequenceCancelRequested_{};
    bool imageSequenceFramesFinished_{};
    bool imageSequencePreRollDone_{};
    bool imageSequenceRestoring_{};
    bool imageSequenceRestoreFractionDone_{};
    std::optional<core::OutputQueue> imageSequenceOutput_;
    std::uint32_t imageSequenceNextFrame_{};
    std::uint32_t imageSequenceSampleIndex_{};
    std::uint32_t imageSequenceSampleCount_{1};
    std::uint64_t imageSequencePreRollFrame_{};
    std::uint64_t imageSequenceRestoreNextFrame_{};
    std::uint64_t imageSequenceRestoreLastWholeFrame_{};
    float imageSequencePreviousSampleFrame_{};
    float imageSequenceRestoreFrame_{};
    double imageSequenceRestoreMediaSeconds_{};
    bool imageSequenceRestorePlaying_{};
    bool imageSequenceRestoreManualCamera_{};
    core::ImageRgba8 imageSequenceImage_;
    std::vector<std::uint64_t> imageSequenceSum_;
    std::string imageSequenceCompletionStatus_;
#endif
};

} // namespace dayo::app
