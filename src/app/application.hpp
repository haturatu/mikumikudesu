#pragma once

#include "graphics/device.hpp"
#include "app/audio_export_job.hpp"
#include "app/video_export_job.hpp"
#include "core/scene.hpp"
#include "core/editor.hpp"
#include "core/project.hpp"
#include "core/output.hpp"
#include "core/video_export.hpp"

#include <cstdint>
#include <array>
#include <filesystem>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace dayo::app {

struct AudioExportOptions {
    std::filesystem::path destination;
    std::optional<std::filesystem::path> source;
    std::uint32_t bitrate { 192'000 };
    std::optional<double> startSeconds;
    std::optional<double> endSeconds;
    bool overwrite {};
};

struct VideoExportOptions {
    std::filesystem::path destination;
    std::optional<std::filesystem::path> audioSource;
    std::uint32_t width { 1920 };
    std::uint32_t height { 1080 };
    double fps { 30.0 };
    core::VideoCodec codec { core::VideoCodec::h264 };
    std::uint32_t bitrate { 8'000'000 };
    std::uint32_t audioBitrate { 192'000 };
    std::optional<std::uint64_t> fromFrame;
    std::optional<std::uint64_t> toFrame;
    bool includeAudio { true };
    bool overwrite {};
};

struct Options {
    bool probeOnly {};
    bool hidden {};
    bool validation { true };
    std::optional<std::uint64_t> frameLimit;
    std::optional<std::filesystem::path> saveProject;
    std::optional<AudioExportOptions> audioExport;
    std::optional<VideoExportOptions> videoExport;
    graphics::RendererKind renderer { graphics::RendererKind::preview };
    std::vector<std::filesystem::path> assets;
};

[[nodiscard]] Options parseOptions(int argc, char** argv);

class Application {
public:
    explicit Application(Options options);
    int run();

private:
    void resetProjectRuntimeState();
    [[nodiscard]] core::DayoProject currentProject() const;
    void handleAsset(const std::filesystem::path& path);
    void refreshAnimatedMesh(bool initialUpload, float deltaSeconds = 0.0F);
    void refreshVideoFrame();
    void refreshPreviewTextures();
    void refreshPreviewBackground();
    void refreshPreviewScene();
    void buildUi();
    void setAudioExportDestinationForSource(const std::filesystem::path& source);
    void buildAudioExportUi();
    void buildVideoExportUi();
    void buildEditorUi();
    int runVideoExport();
    [[nodiscard]] core::ModelInstance* selectedModel() noexcept { return scene_.selectedModel(); }
    [[nodiscard]] const core::ModelInstance* selectedModel() const noexcept { return scene_.selectedModel(); }

    Options options_;
    graphics::Device* device_ {};
    core::Scene scene_;
    core::CommandHistory history_;
    core::AudioPlayer audioPlayer_;
    AudioExportJob audioExportJob_;
    VideoExportJob videoExportJob_;
    std::vector<core::ImageRgba8> textures_;
    float animationFrame_ {};
    int uploadedAnimationFrame_ { -1 };
    bool playing_ { true };
    bool repeat_ { true };
    float playbackSpeed_ { 1.0F };
    float audioVolume_ { 1.0F };
    float audioOffsetSeconds_ {};
    core::AudioBuffer loadedAudio_;
    std::vector<float> waveformPeaks_;
    bool videoMode_ {};
    double mediaSeconds_ {};
    std::int64_t uploadedVideoFrame_ { -1 };
    std::string lastAsset_ { "Drop PMX/VMD/VPD/media files into the window" };
    std::vector<core::ProjectAsset> projectAssets_;
    std::optional<core::EffectHotReloader> effectReloader_;
    core::PreviewNormalization normalization_;
    float cameraYaw_ {};
    float cameraPitch_ {};
    float cameraDistance_ { 3.0F };
    bool manualCamera_ {};
    std::filesystem::path audioSource_;
    std::array<char, 1024> audioDestination_ {};
    int audioBitrateKbps_ { 192 };
    int audioRangeMode_ {};
    float audioFromSeconds_ {};
    float audioToSeconds_ {};
    bool audioOverwrite_ {};
    std::array<char, 1024> videoDestination_ {};
    std::uint32_t videoWidth_ { 1920 };
    std::uint32_t videoHeight_ { 1080 };
    float videoFps_ { 30.0F };
    int videoCodec_ {};
    int videoBitrateKbps_ { 8000 };
    int videoAudioBitrateKbps_ { 192 };
    bool videoIncludeAudio_ { true };
    std::uint64_t videoFromFrame_ {};
    std::uint64_t videoToFrame_ {};
    std::uint64_t videoNextFrame_ {};
    std::uint64_t videoOutputFrameCount_ {};
    double videoSourceFps_ { 30.0 };
    float videoPreviousSourceFrame_ {};
    bool videoPreRollDone_ {};
    bool videoExportFramesFinished_ {};
    bool videoExportUiActive_ {};
    bool videoRangeInitialized_ {};
    std::string videoExportStatus_;
    core::MotionClipboard motionClipboard_;
    std::vector<core::MotionKeyRef> selectedKeys_;
    bool editGlobalMotion_ {};
    bool recordCamera_ {};
    int selectedBone_ {};
    int selectedMorph_ {};
    core::Float3 editedBoneTranslation_ {};
    core::Float4 editedBoneRotation_ { 0.0F, 0.0F, 0.0F, 1.0F };
    bool editedBonePhysics_ { true };
    bool physicsDebug_ {};
    float editedMorphWeight_ {};
    core::VmdCameraKey editedCamera_;
    std::array<char, 256> cameraParentBoneName_ {};
    core::VmdLightKey editedLight_ { 0, { 0.6F, 0.6F, 0.6F }, { -0.5F, -1.0F, 0.5F } };
    core::VmdShadowKey editedShadow_ { 0, 1, 50.0F };
    core::OutputSettings sequenceOutput_;
    std::string sequenceOutputStatus_;
    std::array<char, 1024> projectDestination_ { 'p', 'r', 'o', 'j', 'e', 'c', 't', '.', 'd', 'a', 'y', 'o', '\0' };
    std::string projectSaveStatus_;
};

} // namespace dayo::app
