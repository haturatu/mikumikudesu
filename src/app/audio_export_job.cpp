#include "app/audio_export_job.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace dayo::app {

AudioExportJob::~AudioExportJob() {
    cancel();
}

void AudioExportJob::start(core::AudioExportRequest request) {
    if (running())
        throw std::logic_error("audio export is already running");
    if (worker_.joinable())
        worker_.join();
    {
        std::scoped_lock lock(mutex_);
        error_.reset();
        result_.reset();
    }
    progress_.store(0.0F);
    processedSeconds_.store(0.0);
    totalSeconds_.store(0.0);
    running_.store(true);
    worker_ = std::jthread([this, request = std::move(request)](std::stop_token stopToken) {
        try {
            const auto result = core::exportM4a(
                request,
                [this](const core::AudioExportProgress& progress) {
                    processedSeconds_.store(progress.processedSeconds);
                    totalSeconds_.store(progress.totalSeconds);
                    progress_.store(static_cast<float>(std::clamp(progress.ratio(), 0.0, 1.0)));
                },
                stopToken);
            {
                std::scoped_lock lock(mutex_);
                result_ = result;
            }
            progress_.store(1.0F);
        } catch (const std::exception& exception) {
            if (!stopToken.stop_requested()) {
                std::scoped_lock lock(mutex_);
                error_ = exception.what();
            }
        } catch (...) {
            if (!stopToken.stop_requested()) {
                std::scoped_lock lock(mutex_);
                error_ = "unknown audio export error";
            }
        }
        running_.store(false);
    });
}

void AudioExportJob::cancel() noexcept {
    if (worker_.joinable())
        worker_.request_stop();
}

std::optional<std::string> AudioExportJob::error() const {
    std::scoped_lock lock(mutex_);
    return error_;
}

std::optional<core::AudioExportResult> AudioExportJob::result() const {
    std::scoped_lock lock(mutex_);
    return result_;
}

} // namespace dayo::app
