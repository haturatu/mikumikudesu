#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace dayo::core {

struct ProjectAsset {
    std::string kind;
    std::filesystem::path path;
};

struct DayoProject {
    int version { 2 };
    std::string renderer { "preview" };
    float frame {};
    bool playing { true };
    std::vector<ProjectAsset> assets;
};

// Loads native v2 projects and the asset portion of original MikuMikuDayo
// JSON+binary projects. Returned asset paths are absolute and normalized.
[[nodiscard]] DayoProject loadProject(const std::filesystem::path& path);

// Writes through a temporary file so an interrupted save cannot destroy the
// previous project. Asset paths are made relative to the project directory
// whenever possible.
void saveProject(const std::filesystem::path& path, const DayoProject& project);

} // namespace dayo::core
