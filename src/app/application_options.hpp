#pragma once

#include "core/video_export.hpp"
#include "graphics/device.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace dayo::app {

struct AudioExportOptions {
    std::filesystem::path destination;
    std::optional<std::filesystem::path> source;
    std::uint32_t bitrate{192'000};
    std::optional<double> startSeconds;
    std::optional<double> endSeconds;
    bool overwrite{};
};

struct VideoExportOptions {
    std::filesystem::path destination;
    std::optional<std::filesystem::path> audioSource;
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    double fps{30.0};
    core::VideoCodec codec{core::VideoCodec::h264};
    std::uint32_t bitrate{8'000'000};
    std::uint32_t audioBitrate{192'000};
    bool preferHardware{};
    std::optional<std::uint64_t> fromFrame;
    std::optional<std::uint64_t> toFrame;
    bool includeAudio{true};
    bool overwrite{};
};

struct Options {
    bool probeOnly{};
    bool hidden{};
    bool validation{true};
    std::optional<std::uint64_t> frameLimit;
    std::optional<std::filesystem::path> saveProject;
    std::optional<AudioExportOptions> audioExport;
    std::optional<VideoExportOptions> videoExport;
    graphics::RendererKind renderer{graphics::RendererKind::preview};
    std::vector<std::filesystem::path> assets;
};

} // namespace dayo::app
