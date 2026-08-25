#pragma once

#include "graphics/device.hpp"
#include "core/scene.hpp"
#include "core/editor.hpp"
#include "core/project.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace dayo::app {

struct Options {
    bool probeOnly {};
    bool hidden {};
    bool validation { true };
    std::optional<std::uint64_t> frameLimit;
    std::optional<std::filesystem::path> saveProject;
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
    void refreshPreviewScene();
    void buildUi();
    [[nodiscard]] core::ModelInstance* selectedModel() noexcept { return scene_.selectedModel(); }
    [[nodiscard]] const core::ModelInstance* selectedModel() const noexcept { return scene_.selectedModel(); }

    Options options_;
    graphics::Device* device_ {};
    core::Scene scene_;
    core::CommandHistory history_;
    core::AudioPlayer audioPlayer_;
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
};

} // namespace dayo::app
