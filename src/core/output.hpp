#pragma once

#include "core/image.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace dayo::core {

enum class OutputFormat { ppm, png, exr };

struct OutputSettings {
    std::filesystem::path directory{"output"};
    std::string filenamePattern{"frame_%06d"};
    OutputFormat format{OutputFormat::ppm};
    std::uint32_t firstFrame{};
    std::uint32_t lastFrame{};
    std::uint32_t samples{1};
    std::uint32_t maxPendingFrames{4};
    bool motionBlur{};
};

[[nodiscard]] std::filesystem::path outputPath(const OutputSettings& settings, std::uint32_t frame);
void writeFrame(const std::filesystem::path& path, const ImageRgba8& image, OutputFormat format);

struct OutputWorker;

// The queue decouples rendering from encoding. PPM is dependency-free and is
// the default; applications may plug in PNG/EXR encoders at the boundary.
class OutputQueue {
  public:
    explicit OutputQueue(OutputSettings settings);
    ~OutputQueue();
    OutputQueue(OutputQueue&&) noexcept;
    OutputQueue& operator=(OutputQueue&&) noexcept;
    OutputQueue(const OutputQueue&) = delete;
    OutputQueue& operator=(const OutputQueue&) = delete;

    void push(std::uint32_t frame, ImageRgba8 image);
    void close();
    // Re-throws an encoder/IO failure captured by the worker. It is safe to
    // call after close(), and is intentionally separate so destructors never
    // throw.
    void rethrowIfFailed() const;
    [[nodiscard]] std::uint64_t written() const noexcept;

  private:
    std::unique_ptr<OutputWorker> worker_;
};

} // namespace dayo::core
