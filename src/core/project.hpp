#pragma once

#include "core/motion.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dayo::core {

struct ProjectAsset {
    std::string kind;
    std::filesystem::path path;
    std::optional<std::size_t> ownerModelIndex;
    std::optional<std::int32_t> upstreamId;
    std::vector<std::vector<std::string>> materialSourceFiles;

    ProjectAsset() = default;
    ProjectAsset(std::string assetKind, std::filesystem::path assetPath)
        : kind(std::move(assetKind)), path(std::move(assetPath)) {}
};

struct ProjectModelState {
    std::filesystem::path source;
    std::optional<std::int32_t> upstreamId;
    std::vector<std::string> bones;
    std::vector<std::string> morphs;
    std::vector<std::string> materials;
    std::vector<std::string> materialAnnotations;
    std::int32_t motionOrder{};
    std::int32_t deformOrder{};
    std::int32_t postprocessOrder{};
    std::int32_t rasterOrder{};
    std::uint32_t cloneCount{1};
    bool visible{true};
};

struct ProjectEditorState {
    std::uint32_t samplesPerFrame{16};
    bool motionBlur{};
    std::uint32_t outputWidth{1920};
    std::uint32_t outputHeight{1080};
    float recordFps{30.0F};
    float animationSpeed{1.0F};
};

struct DayoProject {
    // Version 3 is the native format used by the Linux editor. Older
    // Windows projects remain readable through the compatibility reader.
    int version{3};
    std::string renderer{"preview"};
    float frame{};
    bool playing{true};
    std::vector<ProjectAsset> assets;
    std::vector<ProjectModelState> models;
    ProjectEditorState editor;
    // The upstream JSON object is retained verbatim semantically so fields
    // unknown to desu survive a load/save cycle.
    std::string upstreamDocumentJson;
    std::optional<VmdMotion> embeddedMotion;
    // Upstream v3 stores one camera/light subset followed by one subset per
    // model. embeddedMotion remains as the merged single-model compatibility
    // view used by older native projects and API clients.
    std::vector<VmdMotion> embeddedMotions;
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
