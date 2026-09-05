#include "core/media.hpp"

#include "core/ffmpeg_utils.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

#if DAYO_HAS_MEDIA
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace dayo::core {
namespace {

#if DAYO_HAS_MEDIA
AVCodecContext* openDecoder(AVFormatContext* format, int streamIndex) {
    const auto* codec = avcodec_find_decoder(format->streams[streamIndex]->codecpar->codec_id);
    if (codec == nullptr)
        throw std::runtime_error("FFmpeg decoder is unavailable");
    auto* context = avcodec_alloc_context3(codec);
    if (context == nullptr)
        throw std::bad_alloc();
    try {
        ffmpeg::check(avcodec_parameters_to_context(context, format->streams[streamIndex]->codecpar),
                      "copy codec parameters");
        ffmpeg::check(avcodec_open2(context, codec, nullptr), "open codec");
    } catch (...) {
        avcodec_free_context(&context);
        throw;
    }
    return context;
}
#endif

} // namespace

struct MediaFile::Impl {
    MediaInfo info;
#if DAYO_HAS_MEDIA
    AVFormatContext* format{};
    AVCodecContext* audioCodec{};
    AVCodecContext* videoCodec{};
    int audioStream{-1};
    int videoStream{-1};

    ~Impl() {
        avcodec_free_context(&audioCodec);
        avcodec_free_context(&videoCodec);
        avformat_close_input(&format);
    }
#endif
};

MediaFile::MediaFile(const std::filesystem::path& path) : impl_(std::make_unique<Impl>()) {
#if DAYO_HAS_MEDIA
    auto name = path.string();
    ffmpeg::check(avformat_open_input(&impl_->format, name.c_str(), nullptr, nullptr), "open media");
    ffmpeg::check(avformat_find_stream_info(impl_->format, nullptr), "read media streams");
    impl_->audioStream = av_find_best_stream(impl_->format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    impl_->videoStream = av_find_best_stream(impl_->format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (impl_->audioStream >= 0) {
        impl_->audioCodec = openDecoder(impl_->format, impl_->audioStream);
        impl_->info.hasAudio = true;
    }
    if (impl_->videoStream >= 0) {
        impl_->videoCodec = openDecoder(impl_->format, impl_->videoStream);
        impl_->info.hasVideo = true;
        impl_->info.videoWidth = static_cast<std::uint32_t>(impl_->videoCodec->width);
        impl_->info.videoHeight = static_cast<std::uint32_t>(impl_->videoCodec->height);
        const auto rate = av_guess_frame_rate(impl_->format, impl_->format->streams[impl_->videoStream], nullptr);
        impl_->info.videoFramesPerSecond = rate.den != 0 ? av_q2d(rate) : 30.0;
    }
    if (!impl_->info.hasAudio && !impl_->info.hasVideo)
        throw std::runtime_error("media contains no audio or video stream");
    if (impl_->format->duration > 0) {
        impl_->info.durationSeconds = static_cast<double>(impl_->format->duration) / AV_TIME_BASE;
    }
#else
    static_cast<void>(path);
    throw std::runtime_error("FFmpeg support was not built");
#endif
}

MediaFile::~MediaFile() = default;
MediaFile::MediaFile(MediaFile&&) noexcept = default;
MediaFile& MediaFile::operator=(MediaFile&&) noexcept = default;
const MediaInfo& MediaFile::info() const noexcept {
    return impl_->info;
}

AudioBuffer MediaFile::decodeAudio() {
#if DAYO_HAS_MEDIA
    AudioBuffer output;
    streamAudio(
        [&](const std::span<const float> samples, const std::uint32_t sampleRate, const std::uint32_t channels) {
            output.sampleRate = sampleRate;
            output.channels = channels;
            output.samples.insert(output.samples.end(), samples.begin(), samples.end());
        });
    return output;
#else
    throw std::runtime_error("FFmpeg support was not built");
#endif
}

void MediaFile::streamAudio(const AudioSampleCallback& callback, double startSeconds) {
#if DAYO_HAS_MEDIA
    if (impl_->audioCodec == nullptr)
        throw std::runtime_error("media has no audio stream");
    if (!callback)
        throw std::invalid_argument("audio sample callback is empty");
    if (!std::isfinite(startSeconds) || startSeconds < 0.0)
        throw std::invalid_argument("audio start time must be finite and nonnegative");
    auto skipFrames = static_cast<std::uint64_t>(std::round(startSeconds * 48'000.0));
    const auto emit = [&](std::span<const float> samples) {
        const auto skip = std::min<std::uint64_t>(skipFrames, samples.size() / 2);
        skipFrames -= skip;
        samples = samples.subspan(static_cast<std::size_t>(skip) * 2);
        if (!samples.empty())
            callback(samples, 48'000, 2);
    };
    ffmpeg::check(av_seek_frame(impl_->format, impl_->audioStream, 0, AVSEEK_FLAG_BACKWARD), "seek audio");
    avformat_flush(impl_->format);
    avcodec_flush_buffers(impl_->audioCodec);
    AVChannelLayout outputLayout = AV_CHANNEL_LAYOUT_STEREO;
    SwrContext* resampler = nullptr;
    ffmpeg::check(swr_alloc_set_opts2(&resampler, &outputLayout, AV_SAMPLE_FMT_FLT, 48'000,
                                      &impl_->audioCodec->ch_layout, impl_->audioCodec->sample_fmt,
                                      impl_->audioCodec->sample_rate, 0, nullptr),
                  "create audio resampler");
    ffmpeg::check(swr_init(resampler), "initialize audio resampler");
    auto* packet = av_packet_alloc();
    auto* frame = av_frame_alloc();
    if (packet == nullptr || frame == nullptr) {
        av_frame_free(&frame);
        av_packet_free(&packet);
        swr_free(&resampler);
        throw std::bad_alloc();
    }
    try {
        auto receive = [&] {
            while (true) {
                const int result = avcodec_receive_frame(impl_->audioCodec, frame);
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                    break;
                ffmpeg::check(result, "decode audio frame");
                const int capacity =
                    av_rescale_rnd(swr_get_delay(resampler, impl_->audioCodec->sample_rate) + frame->nb_samples, 48'000,
                                   impl_->audioCodec->sample_rate, AV_ROUND_UP);
                std::vector<float> samples(static_cast<std::size_t>(capacity) * 2U);
                std::uint8_t* destination[]{reinterpret_cast<std::uint8_t*>(samples.data())};
                const int converted =
                    swr_convert(resampler, destination, capacity,
                                const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
                ffmpeg::check(converted, "resample audio");
                samples.resize(static_cast<std::size_t>(converted) * 2U);
                if (!samples.empty())
                    emit(samples);
                av_frame_unref(frame);
            }
        };
        while (av_read_frame(impl_->format, packet) >= 0) {
            if (packet->stream_index == impl_->audioStream) {
                ffmpeg::check(avcodec_send_packet(impl_->audioCodec, packet), "submit audio packet");
                receive();
            }
            av_packet_unref(packet);
        }
        ffmpeg::check(avcodec_send_packet(impl_->audioCodec, nullptr), "flush audio decoder");
        receive();
        while (true) {
            const int capacity = av_rescale_rnd(swr_get_delay(resampler, impl_->audioCodec->sample_rate), 48'000,
                                                impl_->audioCodec->sample_rate, AV_ROUND_UP);
            if (capacity <= 0)
                break;
            std::vector<float> samples(static_cast<std::size_t>(capacity) * 2U);
            std::uint8_t* destination[]{reinterpret_cast<std::uint8_t*>(samples.data())};
            const int converted = swr_convert(resampler, destination, capacity, nullptr, 0);
            ffmpeg::check(converted, "flush audio resampler");
            if (converted == 0)
                break;
            samples.resize(static_cast<std::size_t>(converted) * 2U);
            emit(samples);
        }
    } catch (...) {
        av_frame_free(&frame);
        av_packet_free(&packet);
        swr_free(&resampler);
        throw;
    }
    swr_free(&resampler);
    av_frame_free(&frame);
    av_packet_free(&packet);
#else
    static_cast<void>(callback);
    static_cast<void>(startSeconds);
    throw std::runtime_error("FFmpeg support was not built");
#endif
}

ImageRgba8 MediaFile::decodeVideoFrame(double seconds) {
#if DAYO_HAS_MEDIA
    if (impl_->videoCodec == nullptr)
        throw std::runtime_error("media has no video stream");
    const auto* stream = impl_->format->streams[impl_->videoStream];
    const auto timestamp = static_cast<std::int64_t>(seconds / av_q2d(stream->time_base));
    ffmpeg::check(av_seek_frame(impl_->format, impl_->videoStream, timestamp, AVSEEK_FLAG_BACKWARD), "seek video");
    avformat_flush(impl_->format);
    avcodec_flush_buffers(impl_->videoCodec);
    auto* packet = av_packet_alloc();
    auto* frame = av_frame_alloc();
    if (packet == nullptr || frame == nullptr)
        throw std::bad_alloc();
    bool found = false;
    while (!found && av_read_frame(impl_->format, packet) >= 0) {
        if (packet->stream_index == impl_->videoStream) {
            ffmpeg::check(avcodec_send_packet(impl_->videoCodec, packet), "submit video packet");
            while (true) {
                const int result = avcodec_receive_frame(impl_->videoCodec, frame);
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                    break;
                ffmpeg::check(result, "decode video frame");
                const auto pts = frame->best_effort_timestamp;
                if (pts == AV_NOPTS_VALUE || static_cast<double>(pts) * av_q2d(stream->time_base) + 1e-6 >= seconds) {
                    found = true;
                    break;
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);
    }
    if (!found) {
        const int flushResult = avcodec_send_packet(impl_->videoCodec, nullptr);
        if (flushResult >= 0 || flushResult == AVERROR_EOF) {
            while (avcodec_receive_frame(impl_->videoCodec, frame) >= 0) {
                const auto pts = frame->best_effort_timestamp;
                if (pts == AV_NOPTS_VALUE || static_cast<double>(pts) * av_q2d(stream->time_base) + 1e-6 >= seconds) {
                    found = true;
                    break;
                }
                av_frame_unref(frame);
            }
        }
    }
    if (!found) {
        av_frame_free(&frame);
        av_packet_free(&packet);
        throw std::runtime_error("no video frame at requested time");
    }
    ImageRgba8 output;
    output.width = static_cast<std::uint32_t>(frame->width);
    output.height = static_cast<std::uint32_t>(frame->height);
    output.pixels.resize(static_cast<std::size_t>(output.width) * output.height * 4U);
    auto* scaler = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width,
                                  frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (scaler == nullptr)
        throw std::runtime_error("create video color converter failed");
    std::uint8_t* destination[]{output.pixels.data()};
    int stride[]{static_cast<int>(output.width * 4U)};
    sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, destination, stride);
    sws_freeContext(scaler);
    av_frame_free(&frame);
    av_packet_free(&packet);
    return output;
#else
    static_cast<void>(seconds);
    throw std::runtime_error("FFmpeg support was not built");
#endif
}

struct AudioPlayer::Impl {
    SDL_AudioStream* stream{};
};
AudioPlayer::AudioPlayer() : impl_(std::make_unique<Impl>()) {}
AudioPlayer::~AudioPlayer() {
    stop();
}
AudioPlayer::AudioPlayer(AudioPlayer&&) noexcept = default;
AudioPlayer& AudioPlayer::operator=(AudioPlayer&&) noexcept = default;

void AudioPlayer::play(const AudioBuffer& audio, double startSeconds) {
    stop();
    if (audio.samples.empty())
        return;
    if (audio.channels == 0 || audio.sampleRate == 0)
        throw std::invalid_argument("invalid audio format");
    const SDL_AudioSpec spec{SDL_AUDIO_F32, static_cast<int>(audio.channels), static_cast<int>(audio.sampleRate)};
    impl_->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (impl_->stream == nullptr)
        throw std::runtime_error(std::string("open audio device: ") + SDL_GetError());
    const auto totalFrames = audio.samples.size() / audio.channels;
    const auto duration = static_cast<double>(totalFrames) / audio.sampleRate;
    const auto startFrame = static_cast<std::size_t>(std::clamp(startSeconds, 0.0, duration) * audio.sampleRate);
    const auto startSample = std::min(startFrame * audio.channels, audio.samples.size());
    const auto sampleCount = audio.samples.size() - startSample;
    const auto byteCount = sampleCount * sizeof(float);
    if (byteCount > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        !SDL_PutAudioStreamData(impl_->stream, audio.samples.data() + startSample, static_cast<int>(byteCount)) ||
        !SDL_ResumeAudioStreamDevice(impl_->stream)) {
        const std::string error = SDL_GetError();
        stop();
        throw std::runtime_error("play audio: " + error);
    }
}

void AudioPlayer::stop() {
    if (impl_ != nullptr && impl_->stream != nullptr && SDL_WasInit(SDL_INIT_AUDIO) != 0) {
        SDL_DestroyAudioStream(impl_->stream);
    }
    if (impl_ != nullptr)
        impl_->stream = nullptr;
}

void AudioPlayer::setPaused(bool paused) {
    if (impl_->stream == nullptr)
        return;
    const bool success =
        paused ? SDL_PauseAudioStreamDevice(impl_->stream) : SDL_ResumeAudioStreamDevice(impl_->stream);
    if (!success)
        throw std::runtime_error(std::string("change audio pause state: ") + SDL_GetError());
}

void AudioPlayer::setVolume(float volume) {
    if (impl_ == nullptr || impl_->stream == nullptr)
        return;
    SDL_SetAudioStreamGain(impl_->stream, std::clamp(volume, 0.0F, 1.0F));
}

bool AudioPlayer::active() const noexcept {
    return impl_ != nullptr && impl_->stream != nullptr;
}

} // namespace dayo::core
