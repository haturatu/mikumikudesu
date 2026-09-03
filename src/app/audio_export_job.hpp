#pragma once

#include "core/audio_export.hpp"

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace dayo::app {

class AudioExportJob {
  public:
    AudioExportJob() = default;
    ~AudioExportJob();
    AudioExportJob(const AudioExportJob&) = delete;
    AudioExportJob& operator=(const AudioExportJob&) = delete;

    void start(core::AudioExportRequest request);
    void cancel() noexcept;

    [[nodiscard]] bool running() const noexcept {
        return running_.load();
    }
    [[nodiscard]] float progress() const noexcept {
        return progress_.load();
    }
    [[nodiscard]] double processedSeconds() const noexcept {
        return processedSeconds_.load();
    }
    [[nodiscard]] double totalSeconds() const noexcept {
        return totalSeconds_.load();
    }
    [[nodiscard]] std::optional<std::string> error() const;
    [[nodiscard]] std::optional<core::AudioExportResult> result() const;

  private:
    std::jthread worker_;
    std::atomic<bool> running_{};
    std::atomic<float> progress_{};
    std::atomic<double> processedSeconds_{};
    std::atomic<double> totalSeconds_{};
    mutable std::mutex mutex_;
    std::optional<std::string> error_;
    std::optional<core::AudioExportResult> result_;
};

} // namespace dayo::app
