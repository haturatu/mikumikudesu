#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>

namespace dayo::core {

struct AudioExportRequest {
    std::filesystem::path source;
    std::filesystem::path destination;

    std::uint32_t bitrate { 192'000 };
    std::uint32_t sampleRate { 48'000 };
    std::uint32_t channels { 2 };

    std::optional<double> startSeconds;
    std::optional<double> endSeconds;

    bool overwrite {};
};

struct AudioExportProgress {
    double processedSeconds {};
    double totalSeconds {};

    [[nodiscard]] double ratio() const noexcept {
        return totalSeconds > 0.0 ? processedSeconds / totalSeconds : 0.0;
    }
};

struct AudioExportResult {
    std::filesystem::path output;
    double durationSeconds {};
    std::uint64_t encodedSamples {};
};

using AudioExportProgressCallback = std::function<void(const AudioExportProgress&)>;

[[nodiscard]] bool canExportM4a() noexcept;

[[nodiscard]] AudioExportResult exportM4a(
    const AudioExportRequest& request,
    AudioExportProgressCallback progress = {},
    std::stop_token stopToken = {}
);

} // namespace dayo::core
