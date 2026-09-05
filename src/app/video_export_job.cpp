#include "app/video_export_job.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace dayo::app {

VideoExportJob::~VideoExportJob() {
    cancel();
}

void VideoExportJob::start(core::VideoExportRequest request, std::optional<std::filesystem::path> audioSource,
                           std::uint64_t totalFrames) {
    cancel();
    if (totalFrames == 0)
        throw std::invalid_argument("video export has no frames");
    {
        std::lock_guard lock(queueMutex_);
        frames_.clear();
        inputFinished_ = false;
        totalFrames_ = totalFrames;
        processedFrames_ = 0;
    }
    {
        std::lock_guard lock(resultMutex_);
        error_.reset();
        result_.reset();
    }
    progress_ = 0.0F;
    running_ = true;
    worker_ =
        std::jthread([this, request = std::move(request), audioSource = std::move(audioSource)](std::stop_token token) {
            try {
                core::VideoExporter exporter(request);
                if (audioSource && request.includeAudio) {
                    core::MediaFile media(*audioSource);
                    const auto maxSamples = static_cast<std::uint64_t>(
                                                std::ceil(static_cast<double>(totalFrames_) / request.fps * 48'000.0)) *
                                            2U;
                    std::uint64_t writtenSamples = 0;
                    media.streamAudio(
                        [&](std::span<const float> samples, std::uint32_t sampleRate, std::uint32_t channels) {
                            if (token.stop_requested())
                                throw std::runtime_error("video export cancelled");
                            if (writtenSamples >= maxSamples)
                                return;
                            const auto count = std::min<std::uint64_t>(samples.size(), maxSamples - writtenSamples);
                            exporter.writeAudio(samples.first(static_cast<std::size_t>(count)), sampleRate, channels);
                            writtenSamples += count;
                        },
                        request.audioStartSeconds);
                }
                while (true) {
                    core::ImageRgba8 frame;
                    {
                        std::unique_lock lock(queueMutex_);
                        queueChanged_.wait(lock, token, [this] { return !frames_.empty() || inputFinished_; });
                        if (token.stop_requested())
                            return;
                        if (frames_.empty() && inputFinished_)
                            break;
                        frame = std::move(frames_.front());
                        frames_.pop_front();
                        queueChanged_.notify_all();
                    }
                    exporter.writeVideoFrame(frame);
                    ++processedFrames_;
                    progress_ = static_cast<float>(processedFrames_) / static_cast<float>(totalFrames_);
                }
                const auto result = exporter.finish();
                {
                    std::lock_guard lock(resultMutex_);
                    result_ = result;
                }
            } catch (const std::exception& exception) {
                if (!token.stop_requested()) {
                    std::lock_guard lock(resultMutex_);
                    error_ = exception.what();
                }
            }
            running_ = false;
            queueChanged_.notify_all();
        });
}

void VideoExportJob::submitFrame(core::ImageRgba8 frame) {
    std::unique_lock lock(queueMutex_);
    queueChanged_.wait(lock, [this] { return frames_.size() < kFrameQueueCapacity || !running_.load(); });
    if (!running_)
        throw std::runtime_error("video export is not running");
    frames_.push_back(std::move(frame));
    queueChanged_.notify_all();
}

void VideoExportJob::finishFrames() {
    {
        std::lock_guard lock(queueMutex_);
        inputFinished_ = true;
    }
    queueChanged_.notify_all();
}

void VideoExportJob::cancel() noexcept {
    if (worker_.joinable()) {
        worker_.request_stop();
        {
            std::lock_guard lock(queueMutex_);
            inputFinished_ = true;
        }
        queueChanged_.notify_all();
        worker_.join();
    }
    running_ = false;
}

std::optional<std::string> VideoExportJob::error() const {
    std::lock_guard lock(resultMutex_);
    return error_;
}

std::optional<core::VideoExportResult> VideoExportJob::result() const {
    std::lock_guard lock(resultMutex_);
    return result_;
}

} // namespace dayo::app
