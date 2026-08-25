#pragma once

#include "core/motion.hpp"

#include <filesystem>
#include <cstdint>
#include <string>
#include <optional>
#include <vector>

namespace dayo::core {

struct ProjectAsset {
    std::string kind;
    std::filesystem::path path;
};

struct DayoProject {
    // Version 3 is the native format used by the Linux editor. Older
    // Windows projects remain readable through the compatibility reader.
    int version { 3 };
    std::string renderer { "preview" };
    float frame {};
    bool playing { true };
    std::vector<ProjectAsset> assets;
    std::optional<VmdMotion> embeddedMotion;
    // v3 stores an opaque upstream keyframe payload verbatim. Keeping bytes
    // losslessly is safer than claiming compatibility with an unknown format.
    std::vector<std::uint8_t> embeddedVmdayo;
};

// Loads native v2 projects and the asset portion of original MikuMikuDayo
// JSON+binary projects. Returned asset paths are absolute and normalized.
[[nodiscard]] DayoProject loadProject(const std::filesystem::path& path);

// Writes through a temporary file so an interrupted save cannot destroy the
// previous project. Asset paths are made relative to the project directory
// whenever possible.
void saveProject(const std::filesystem::path& path, const DayoProject& project);

} // namespace dayo::core
