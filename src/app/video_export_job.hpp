#pragma once

#include "core/media.hpp"
#include "core/video_export.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace dayo::app {

class VideoExportJob {
public:
    VideoExportJob() = default;
    ~VideoExportJob();
    VideoExportJob(const VideoExportJob&) = delete;
    VideoExportJob& operator=(const VideoExportJob&) = delete;

    void start(core::VideoExportRequest request,
               std::optional<std::filesystem::path> audioSource,
               std::uint64_t totalFrames);
    void submitFrame(core::ImageRgba8 frame);
    void finishFrames();
    void cancel() noexcept;

    [[nodiscard]] bool running() const noexcept { return running_.load(); }
    [[nodiscard]] float progress() const noexcept { return progress_.load(); }
    [[nodiscard]] std::optional<std::string> error() const;
    [[nodiscard]] std::optional<core::VideoExportResult> result() const;

private:
    std::jthread worker_;
    std::atomic<bool> running_ {};
    std::atomic<float> progress_ {};
    std::mutex queueMutex_;
    std::condition_variable_any queueChanged_;
    std::deque<core::ImageRgba8> frames_;
    bool inputFinished_ {};
    std::uint64_t totalFrames_ {};
    std::uint64_t processedFrames_ {};
    mutable std::mutex resultMutex_;
    std::optional<std::string> error_;
    std::optional<core::VideoExportResult> result_;
};

} // namespace dayo::app
