#include "app/video_export_job.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

int main() {
    if (!dayo::core::canExportVideo())
        return 0;
    try {
        dayo::app::VideoExportJob job;
        dayo::core::VideoExportRequest request;
        request.destination = std::filesystem::temp_directory_path() / "dayo-video-job-test.mp4";
        request.width = request.height = 16;
        request.includeAudio = false;
        request.overwrite = true;
        constexpr std::uint64_t frameCount = 30;
        job.start(request, std::nullopt, frameCount);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::uint64_t submitted = 0;
        while (submitted < frameCount && job.running() && std::chrono::steady_clock::now() < deadline) {
            dayo::core::ImageRgba8 image{16, 16, std::vector<std::uint8_t>(16 * 16 * 4, 255)};
            if (job.trySubmitFrame(std::move(image)))
                ++submitted;
            else if (image.pixels.empty())
                throw std::runtime_error("full queue consumed the caller's frame");
            else
                std::this_thread::yield();
        }
        job.finishFrames();
        while (job.running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        if (const auto error = job.error())
            throw std::runtime_error(*error);
        const auto result = job.result();
        if (submitted != frameCount || job.running() || !result || result->encodedFrames != frameCount ||
            job.canAcceptFrame())
            throw std::runtime_error("video export did not drain all submitted frames");

        // Cancellation must also terminate when the worker is waiting for input.
        job.start(request, std::nullopt, frameCount);
        job.requestCancel();
        const auto cancelDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (job.running() && std::chrono::steady_clock::now() < cancelDeadline)
            std::this_thread::yield();
        if (job.running() || job.canAcceptFrame())
            throw std::runtime_error("cancelled video job remained active");
        job.cancel();
        std::filesystem::remove(request.destination);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
