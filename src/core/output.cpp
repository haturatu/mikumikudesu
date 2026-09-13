#include "core/output.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#if DAYO_HAS_OPENEXR
#include <OpenEXR/ImfArray.h>
#include <OpenEXR/ImfRgbaFile.h>
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>

namespace dayo::core {

namespace {

OutputSettings normalizeSettings(OutputSettings settings) {
    settings.maxPendingFrames = std::max(settings.maxPendingFrames, 1U);
    if (settings.lastFrame < settings.firstFrame)
        throw std::invalid_argument("output frame range is reversed");
    if (!settings.overwrite) {
        for (std::uint64_t frame = settings.firstFrame; frame <= settings.lastFrame; ++frame) {
            const auto path = outputPath(settings, static_cast<std::uint32_t>(frame));
            if (std::filesystem::exists(path))
                throw std::runtime_error(
                    path.string() + " already exists. Enable Overwrite existing frames or choose another directory.");
        }
    }
    return settings;
}

} // namespace

std::filesystem::path outputPath(const OutputSettings& settings, std::uint32_t frame) {
    char name[256]{};
    std::snprintf(name, sizeof(name), settings.filenamePattern.c_str(), frame);
    const auto extension = settings.format == OutputFormat::png   ? ".png"
                           : settings.format == OutputFormat::exr ? ".exr"
                                                                  : ".ppm";
    return settings.directory / (std::string(name) + extension);
}

void writeFrame(const std::filesystem::path& path, const ImageRgba8& image, OutputFormat format) {
    std::filesystem::create_directories(path.parent_path());
    if (format == OutputFormat::png) {
        if (stbi_write_png(path.c_str(), static_cast<int>(image.width), static_cast<int>(image.height), 4,
                           image.pixels.data(), static_cast<int>(image.width * 4U)) == 0) {
            throw std::runtime_error("cannot encode PNG frame: " + path.string());
        }
        return;
    }
    if (format == OutputFormat::exr) {
        if (image.width == 0 || image.height == 0 || image.pixels.empty())
            throw std::invalid_argument("cannot encode an empty EXR frame");
        ImageData source{
            .width = image.width,
            .height = image.height,
            .channels = 4,
            .type = PixelType::unorm8,
            .space = ColorSpace::srgb,
            .bytes = image.pixels,
        };
        writeFrame(path, convertImage(source, PixelType::half16, ColorSpace::linear));
        return;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot write output frame: " + path.string());
    output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (std::size_t index = 0; index + 3 < image.pixels.size(); index += 4) {
        output.put(static_cast<char>(image.pixels[index]));
        output.put(static_cast<char>(image.pixels[index + 1]));
        output.put(static_cast<char>(image.pixels[index + 2]));
    }
}

void writeFrame(const std::filesystem::path& path, const ImageData& image) {
    if (image.width == 0 || image.height == 0 || image.channels != 4 || image.bytes.empty())
        throw std::invalid_argument("EXR input must be a non-empty RGBA image");
    if (image.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        image.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("EXR dimensions exceed the encoder limit");
#if DAYO_HAS_OPENEXR
    const ImageData linear = image.type == PixelType::half16 && image.space == ColorSpace::linear
                                 ? image
                                 : convertImage(image, PixelType::half16, ColorSpace::linear);
    const auto pixels = static_cast<std::size_t>(linear.width) * linear.height;
    if (linear.bytes.size() < pixels * 4U * sizeof(std::uint16_t))
        throw std::invalid_argument("truncated RGBA half image for EXR");
    Imf::Array2D<Imf::Rgba> outputPixels;
    outputPixels.resizeErase(static_cast<int>(linear.height), static_cast<int>(linear.width));
    for (std::uint32_t y = 0; y < linear.height; ++y) {
        for (std::uint32_t x = 0; x < linear.width; ++x) {
            Imf::Rgba pixel;
            std::memcpy(&pixel,
                        linear.bytes.data() +
                            (static_cast<std::size_t>(y) * linear.width + x) * 4U * sizeof(std::uint16_t),
                        sizeof(pixel));
            outputPixels[static_cast<int>(y)][static_cast<int>(x)] = pixel;
        }
    }
    std::filesystem::create_directories(path.parent_path());
    Imf::RgbaOutputFile output(path.string().c_str(), static_cast<int>(linear.width), static_cast<int>(linear.height),
                               Imf::WRITE_RGBA);
    output.setFrameBuffer(&outputPixels[0][0], 1, static_cast<std::size_t>(linear.width));
    output.writePixels(static_cast<int>(linear.height));
#else
    static_cast<void>(path);
    static_cast<void>(image);
    throw std::runtime_error("EXR encoding requires an OpenEXR-enabled build; use PNG or PPM");
#endif
}

struct OutputWorker {
    struct Item {
        std::uint32_t frame;
        ImageRgba8 image;
    };
    explicit OutputWorker(OutputSettings value)
        : settings(normalizeSettings(std::move(value))), thread([this] { run(); }) {}
    ~OutputWorker() {
        close();
    }
    void run() noexcept {
        try {
            for (;;) {
                Item item;
                {
                    std::unique_lock lock(mutex);
                    condition.wait(lock, [this] { return done || !queue.empty(); });
                    if (queue.empty() && done) {
                        finished = true;
                        return;
                    }
                    item = std::move(queue.front());
                    queue.pop();
                    condition.notify_all();
                }
                const auto path = outputPath(settings, item.frame);
                if (!settings.overwrite && std::filesystem::exists(path))
                    throw std::runtime_error(path.string() + " already exists; frame was not overwritten");
                writeFrame(path, item.image, settings.format);
                count.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) {
            {
                std::lock_guard lock(mutex);
                error = std::current_exception();
                done = true;
                std::queue<Item> discarded;
                queue.swap(discarded);
            }
            condition.notify_all();
            finished = true;
        }
    }
    void push(Item item) {
        std::unique_lock lock(mutex);
        condition.wait(lock, [this] { return done || queue.size() < settings.maxPendingFrames; });
        if (done)
            throw std::runtime_error("output queue is closed");
        queue.push(std::move(item));
        lock.unlock();
        condition.notify_one();
    }
    void close() {
        {
            std::lock_guard lock(mutex);
            done = true;
        }
        condition.notify_all();
        if (thread.joinable())
            thread.join();
    }
    OutputSettings settings;
    std::queue<Item> queue;
    std::mutex mutex;
    std::condition_variable condition;
    bool done{};
    std::atomic_uint64_t count{};
    std::atomic<bool> finished{};
    std::exception_ptr error;
    // Start only after every field accessed by run() has been initialized.
    std::thread thread;
};

OutputQueue::OutputQueue(OutputSettings settings) : worker_(std::make_unique<OutputWorker>(std::move(settings))) {}
OutputQueue::~OutputQueue() = default;
OutputQueue::OutputQueue(OutputQueue&&) noexcept = default;
OutputQueue& OutputQueue::operator=(OutputQueue&&) noexcept = default;
void OutputQueue::push(std::uint32_t frame, ImageRgba8 image) {
    worker_->push({frame, std::move(image)});
}
bool OutputQueue::canAcceptFrame() const {
    rethrowIfFailed();
    std::lock_guard lock(worker_->mutex);
    return !worker_->done && worker_->queue.size() < worker_->settings.maxPendingFrames;
}
bool OutputQueue::tryPush(std::uint32_t frame, ImageRgba8&& image) {
    rethrowIfFailed();
    std::lock_guard lock(worker_->mutex);
    if (worker_->done)
        throw std::runtime_error("output queue is closed");
    if (worker_->queue.size() >= worker_->settings.maxPendingFrames)
        return false;
    worker_->queue.push({frame, std::move(image)});
    worker_->condition.notify_one();
    return true;
}
void OutputQueue::requestClose() {
    std::lock_guard lock(worker_->mutex);
    worker_->done = true;
    worker_->condition.notify_all();
}
bool OutputQueue::finished() const {
    return worker_->finished.load();
}
void OutputQueue::close() {
    if (worker_)
        worker_->close();
}
void OutputQueue::rethrowIfFailed() const {
    if (worker_ == nullptr)
        return;
    std::exception_ptr error;
    {
        std::lock_guard lock(worker_->mutex);
        error = worker_->error;
    }
    if (error)
        std::rethrow_exception(error);
}
std::uint64_t OutputQueue::written() const noexcept {
    return worker_ == nullptr ? 0 : worker_->count.load(std::memory_order_relaxed);
}

} // namespace dayo::core
