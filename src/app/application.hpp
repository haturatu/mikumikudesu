#pragma once

#include "graphics/device.hpp"
#include "app/audio_export_job.hpp"
#include "core/scene.hpp"
#include "core/editor.hpp"
#include "core/project.hpp"

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

struct Options {
    bool probeOnly {};
    bool hidden {};
    bool validation { true };
    std::optional<std::uint64_t> frameLimit;
    std::optional<std::filesystem::path> saveProject;
    std::optional<AudioExportOptions> audioExport;
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
    void handleAsset(const std::filesystem::path& path);
    void refreshAnimatedMesh(bool initialUpload, float deltaSeconds = 0.0F);
    void refreshVideoFrame();
    void refreshPreviewTextures();
    void refreshPreviewBackground();
    void refreshPreviewScene();
    void buildUi();
    void setAudioExportDestinationForSource(const std::filesystem::path& source);
    void buildAudioExportUi();
    [[nodiscard]] core::ModelInstance* selectedModel() noexcept { return scene_.selectedModel(); }
    [[nodiscard]] const core::ModelInstance* selectedModel() const noexcept { return scene_.selectedModel(); }

    Options options_;
    graphics::Device* device_ {};
    core::Scene scene_;
    core::CommandHistory history_;
    core::AudioPlayer audioPlayer_;
    AudioExportJob audioExportJob_;
    std::vector<core::ImageRgba8> textures_;
    float animationFrame_ {};
    int uploadedAnimationFrame_ { -1 };
    bool playing_ { true };
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
};

} // namespace dayo::app
