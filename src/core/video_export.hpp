#pragma once

#include "core/image.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace dayo::core {

enum class VideoCodec { h264, h265, av1 };

struct VideoExportRequest {
    std::filesystem::path destination;
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    double fps{30.0};
    VideoCodec codec{VideoCodec::h264};
    std::uint32_t bitrate{8'000'000};
    bool includeAudio{true};
    std::uint32_t audioBitrate{192'000};
    bool preferHardware{};
    bool overwrite{};
    double audioStartSeconds{};
};

struct VideoExportProgress {
    std::uint64_t processedFrames{};
    std::uint64_t totalFrames{};

    [[nodiscard]] double ratio() const noexcept {
        return totalFrames > 0 ? static_cast<double>(processedFrames) / static_cast<double>(totalFrames) : 0.0;
    }
};

struct VideoExportResult {
    std::filesystem::path output;
    std::uint64_t encodedFrames{};
    double durationSeconds{};
};

[[nodiscard]] bool canExportVideo(VideoCodec codec = VideoCodec::h264) noexcept;
[[nodiscard]] bool canExportVideoHardware(VideoCodec codec = VideoCodec::h264) noexcept;

class VideoExporter {
  public:
    explicit VideoExporter(const VideoExportRequest& request);
    ~VideoExporter();
    VideoExporter(VideoExporter&&) noexcept;
    VideoExporter& operator=(VideoExporter&&) noexcept;
    VideoExporter(const VideoExporter&) = delete;
    VideoExporter& operator=(const VideoExporter&) = delete;

    void writeVideoFrame(const ImageRgba8& image);
    // Samples are interleaved float PCM. The input format may differ from the
    // AAC output format; the exporter resamples it incrementally.
    void writeAudio(std::span<const float> samples, std::uint32_t sampleRate = 48'000, std::uint32_t channels = 2);
    [[nodiscard]] VideoExportResult finish();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dayo::core
