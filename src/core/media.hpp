#pragma once

#include "core/image.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace dayo::core {

struct MediaInfo {
    bool hasAudio {};
    bool hasVideo {};
    double durationSeconds {};
    double videoFramesPerSecond {};
    std::uint32_t videoWidth {};
    std::uint32_t videoHeight {};
};

struct AudioBuffer {
    std::uint32_t sampleRate { 48'000 };
    std::uint32_t channels { 2 };
    std::vector<float> samples;
};

using AudioSampleCallback =
    std::function<void(std::span<const float> samples,
                       std::uint32_t sampleRate,
                       std::uint32_t channels)>;

class MediaFile {
public:
    explicit MediaFile(const std::filesystem::path& path);
    ~MediaFile();
    MediaFile(MediaFile&&) noexcept;
    MediaFile& operator=(MediaFile&&) noexcept;
    MediaFile(const MediaFile&) = delete;
    MediaFile& operator=(const MediaFile&) = delete;

    [[nodiscard]] const MediaInfo& info() const noexcept;
    [[nodiscard]] AudioBuffer decodeAudio();
    void streamAudio(const AudioSampleCallback& callback);
    [[nodiscard]] ImageRgba8 decodeVideoFrame(double seconds);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();
    AudioPlayer(AudioPlayer&&) noexcept;
    AudioPlayer& operator=(AudioPlayer&&) noexcept;
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    void play(const AudioBuffer& audio);
    void stop();
    void setPaused(bool paused);
    [[nodiscard]] bool active() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dayo::core
