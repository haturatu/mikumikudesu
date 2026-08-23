#pragma once

#include "graphics/device.hpp"
#include "core/animation.hpp"
#include "core/image.hpp"
#include "core/physics.hpp"

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
    graphics::RendererKind renderer { graphics::RendererKind::preview };
    std::vector<std::filesystem::path> assets;
};

[[nodiscard]] Options parseOptions(int argc, char** argv);

class Application {
public:
    explicit Application(Options options);
    int run();

private:
    void handleAsset(const std::filesystem::path& path);
    void refreshAnimatedMesh(bool initialUpload, float deltaSeconds = 0.0F);
    void buildUi();

    Options options_;
    graphics::Device* device_ {};
    std::unique_ptr<core::PmxModel> model_;
    std::unique_ptr<core::VmdMotion> motion_;
    std::unique_ptr<core::VpdPose> pose_;
    std::unique_ptr<core::MmdAnimator> animator_;
    std::unique_ptr<core::MmdPhysics> physics_;
    std::vector<core::ImageRgba8> textures_;
    float animationFrame_ {};
    int uploadedAnimationFrame_ { -1 };
    bool playing_ { true };
    std::string lastAsset_ { "Drop PMX/VMD/VPD/media files into the window" };
};

} // namespace dayo::app
