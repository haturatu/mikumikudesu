#pragma once

#include "graphics/device.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
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
    void buildUi();

    Options options_;
    std::string lastAsset_ { "Drop PMX/VMD/VPD/media files into the window" };
};

} // namespace dayo::app

