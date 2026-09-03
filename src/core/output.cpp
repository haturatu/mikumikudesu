#include "core/output.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <fstream>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>

namespace dayo::core {

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
        throw std::runtime_error("EXR encoding requires an OpenEXR-enabled build; use PNG or PPM");
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

struct OutputWorker {
    struct Item {
        std::uint32_t frame;
        ImageRgba8 image;
    };
    explicit OutputWorker(OutputSettings value) : settings(std::move(value)), thread([this] { run(); }) {}
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
                    if (queue.empty() && done)
                        return;
                    item = std::move(queue.front());
                    queue.pop();
                }
                writeFrame(outputPath(settings, item.frame), item.image, settings.format);
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
        }
    }
    void push(Item item) {
        {
            std::lock_guard lock(mutex);
            if (done)
                throw std::runtime_error("output queue is closed");
            queue.push(std::move(item));
        }
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
    std::thread thread;
    bool done{};
    std::atomic_uint64_t count{};
    std::exception_ptr error;
};

OutputQueue::OutputQueue(OutputSettings settings) : worker_(std::make_unique<OutputWorker>(std::move(settings))) {}
OutputQueue::~OutputQueue() = default;
OutputQueue::OutputQueue(OutputQueue&&) noexcept = default;
OutputQueue& OutputQueue::operator=(OutputQueue&&) noexcept = default;
void OutputQueue::push(std::uint32_t frame, ImageRgba8 image) {
    worker_->push({frame, std::move(image)});
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
