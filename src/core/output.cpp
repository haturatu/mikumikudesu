#include "core/output.hpp"
#include "core/sequence_path.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#if DAYO_HAS_OPENEXR
#include <OpenEXR/ImfArray.h>
#include <OpenEXR/ImfRgbaFile.h>
#endif

#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <queue>
#include <random>
#include <stdexcept>
#include <thread>

namespace dayo::core {

namespace {

std::string outputExtension(OutputFormat format) {
    return format == OutputFormat::png ? ".png" : format == OutputFormat::exr ? ".exr" : ".ppm";
}
SequencePathSpec dayoSequence(const OutputSettings& settings) {
    const auto filename = settings.sequenceFile;
    if (filename.empty() || filename.has_parent_path() || filename.stem().empty())
        throw std::invalid_argument("sequence filename must be a filename without a directory");
    SequencePathSpec spec;
    const auto stem = filename.stem().string();
    if (!stem.empty() && stem.back() >= '0' && stem.back() <= '9') {
        const auto parsed = parseSequencePath(std::filesystem::path(stem + ".tmp"));
        if (!parsed)
            throw std::invalid_argument("sequence filename number exceeds uint32 range");
        spec = *parsed;
    } else {
        spec.prefix = stem;
        spec.start = 0;
    }
    spec.digits = 5;
    spec.extension = outputExtension(settings.format);
    return spec;
}
std::string legacyOutputName(std::string_view pattern, std::uint32_t frame) {
    std::string result;
    bool numbered = false;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%') {
            result += pattern[i];
            continue;
        }
        if (++i == pattern.size())
            throw std::invalid_argument("incomplete output filename conversion");
        if (pattern[i] == '%') {
            result += '%';
            continue;
        }
        if (numbered)
            throw std::invalid_argument("output filename requires exactly one number conversion");
        const bool zeroPad = pattern[i] == '0';
        if (zeroPad)
            ++i;
        std::uint32_t width = 0;
        while (i < pattern.size() && pattern[i] >= '0' && pattern[i] <= '9') {
            width = width * 10U + static_cast<std::uint32_t>(pattern[i++] - '0');
            if (width > 1024)
                throw std::invalid_argument("output filename width exceeds 1024");
        }
        if (i >= pattern.size() || (pattern[i] != 'u' && pattern[i] != 'd'))
            throw std::invalid_argument("output filename supports only %d, %u and integer widths");
        auto number = std::to_string(frame);
        if (number.size() < width)
            number.insert(0, width - number.size(), zeroPad ? '0' : ' ');
        result += number;
        numbered = true;
    }
    if (!numbered)
        throw std::invalid_argument("output filename requires a number conversion");
    return result;
}

// Encode to a same-filesystem temporary file, then publish using a hard link.
// Creation is atomic and cannot replace an existing frame, even with another
// process writing to the output directory. The worker advances Dayo numbering
// on a collision. Unsupported filesystems report an IO error.
struct EncodedFrame {
    std::filesystem::path directory;
    std::filesystem::path file;
    EncodedFrame(const std::filesystem::path& outputDirectory, const ImageRgba8& image, OutputFormat format) {
        const auto parent = outputDirectory.empty() ? std::filesystem::path(".") : outputDirectory;
        std::filesystem::create_directories(parent);
        std::random_device random;
        for (unsigned attempt = 0; attempt < 1024; ++attempt) {
            directory = parent / (".dayo-frame-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (std::filesystem::create_directory(directory))
                break;
            directory.clear();
        }
        if (directory.empty())
            throw std::runtime_error("cannot reserve temporary sequence directory");
        file = directory / ("frame" + outputExtension(format));
        try {
            writeFrame(file, image, format);
        } catch (...) {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
            throw;
        }
    }
    ~EncodedFrame() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};

OutputSettings normalizeSettings(OutputSettings settings) {
    settings.maxPendingFrames = std::max(settings.maxPendingFrames, 1U);
    if (settings.lastFrame < settings.firstFrame)
        throw std::invalid_argument("output frame range is reversed");
#if !DAYO_HAS_OPENEXR
    if (settings.format == OutputFormat::exr)
        throw std::runtime_error("EXR output requires an OpenEXR-enabled build");
#endif
    if (!settings.sequenceFile.empty()) {
        static_cast<void>(dayoSequence(settings));
        static_cast<void>(firstSequenceOutputPath(settings));
    } else if (!settings.overwrite) {
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
    if (!settings.sequenceFile.empty()) {
        const auto spec = dayoSequence(settings);
        if (frame < settings.firstFrame)
            throw std::invalid_argument("output frame precedes sequence start");
        const auto number = static_cast<std::uint64_t>(spec.start) + frame - settings.firstFrame;
        if (number > UINT32_MAX)
            throw std::overflow_error("sequence number exhausted");
        return formatSequencePath(settings.directory, spec, static_cast<std::uint32_t>(number));
    }
    return settings.directory / (legacyOutputName(settings.filenamePattern, frame) + outputExtension(settings.format));
}

std::filesystem::path firstSequenceOutputPath(const OutputSettings& settings) {
    if (settings.sequenceFile.empty())
        return outputPath(settings, settings.firstFrame);
    const auto spec = dayoSequence(settings);
    for (std::uint64_t number = spec.start; number <= UINT32_MAX; ++number) {
        const auto path = formatSequencePath(settings.directory, spec, static_cast<std::uint32_t>(number));
        if (settings.overwrite || !std::filesystem::exists(path))
            return path;
    }
    throw std::overflow_error("sequence number exhausted");
}

void writeFrame(const std::filesystem::path& path, const ImageRgba8& image, OutputFormat format) {
    if (!path.parent_path().empty())
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
    if (!path.parent_path().empty())
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
        : settings(normalizeSettings(std::move(value))),
          nextNumber(settings.sequenceFile.empty() ? 0 : dayoSequence(settings).start), thread([this] { run(); }) {}
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
                const auto spec =
                    settings.sequenceFile.empty() ? std::optional<SequencePathSpec>{} : dayoSequence(settings);
                const auto candidate = [&]() {
                    if (!spec)
                        return outputPath(settings, item.frame);
                    if (nextNumber > UINT32_MAX)
                        throw std::overflow_error("sequence number exhausted");
                    return formatSequencePath(settings.directory, *spec, static_cast<std::uint32_t>(nextNumber));
                };
                auto path = candidate();
                if (!spec) {
                    if (!settings.overwrite && std::filesystem::exists(path))
                        throw std::runtime_error("output frame already exists: " + path.string());
                    // Keep filenamePattern output on the regular encoder path.
                    // Only Dayo sequence publication needs a hard link to claim
                    // the next available sequence number without replacing it.
                    writeFrame(path, item.image, settings.format);
                } else if (settings.overwrite) {
                    writeFrame(path, item.image, settings.format);
                    ++nextNumber;
                } else {
                    EncodedFrame encoded(path.parent_path(), item.image, settings.format);
                    for (;;) {
                        std::error_code publicationError;
                        std::filesystem::create_hard_link(encoded.file, path, publicationError);
                        if (!publicationError) {
                            if (spec)
                                ++nextNumber;
                            break;
                        }
                        if (publicationError != std::errc::file_exists || !spec)
                            throw std::runtime_error("cannot publish output frame " + path.string() + ": " +
                                                     publicationError.message());
                        ++nextNumber;
                        path = candidate();
                    }
                }
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
    std::uint64_t nextNumber{};
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
