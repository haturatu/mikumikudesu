#include "core/video_export.hpp"

#include "core/ffmpeg_utils.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if DAYO_HAS_MEDIA
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace dayo::core {
namespace {

#if DAYO_HAS_MEDIA
const AVCodec* findVideoEncoder(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::h264:
        return avcodec_find_encoder(AV_CODEC_ID_H264);
    case VideoCodec::h265:
        return avcodec_find_encoder(AV_CODEC_ID_HEVC);
    case VideoCodec::av1:
        return avcodec_find_encoder(AV_CODEC_ID_AV1);
    }
    return nullptr;
}

AVCodecID videoCodecId(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::h264:
        return AV_CODEC_ID_H264;
    case VideoCodec::h265:
        return AV_CODEC_ID_HEVC;
    case VideoCodec::av1:
        return AV_CODEC_ID_AV1;
    }
    return AV_CODEC_ID_NONE;
}

void freeFrame(AVFrame*& frame) noexcept {
    av_frame_free(&frame);
}
void freePacket(AVPacket*& packet) noexcept {
    av_packet_free(&packet);
}

#endif

} // namespace

struct VideoExporter::Impl {
    explicit Impl(const VideoExportRequest& request) : request(request) {
#if DAYO_HAS_MEDIA
        initialize();
#else
        static_cast<void>(request);
        throw std::runtime_error("FFmpeg support was not built");
#endif
    }

    ~Impl() {
#if DAYO_HAS_MEDIA
        close(false);
#endif
    }

#if DAYO_HAS_MEDIA
    VideoExportRequest request;
    std::filesystem::path partPath;
    AVFormatContext* format{};
    AVCodecContext* videoCodec{};
    AVCodecContext* audioCodec{};
    AVStream* videoStream{};
    AVStream* audioStream{};
    SwsContext* scaler{};
    SwrContext* resampler{};
    AVFrame* videoFrame{};
    AVPacket* packet{};
    AVPacket* pendingAudioPacket{};
    std::array<std::vector<float>, 2> pendingAudio_;
    std::uint64_t videoFrames{};
    std::uint64_t audioSamples{};
    std::uint64_t audioTargetSamples{};
    bool headerWritten{};
    bool finished{};

    void initialize() {
        if (request.width == 0 || request.height == 0) {
            throw std::invalid_argument("video dimensions must be non-zero");
        }
        if (!std::isfinite(request.fps) || request.fps <= 0.0) {
            throw std::invalid_argument("video FPS must be finite and positive");
        }
        if (request.bitrate == 0)
            throw std::invalid_argument("video bitrate must be positive");
        if (request.includeAudio && request.audioBitrate == 0) {
            throw std::invalid_argument("audio bitrate must be positive");
        }
        if (request.destination.empty())
            throw std::invalid_argument("video destination is empty");
        if (std::filesystem::exists(request.destination) && !request.overwrite) {
            throw std::runtime_error("output already exists: " + request.destination.string());
        }
        if (const auto parent = request.destination.parent_path(); !parent.empty()) {
            std::error_code error;
            std::filesystem::create_directories(parent, error);
            if (error)
                throw std::runtime_error("create video output directory: " + error.message());
        }
        partPath = request.destination;
        partPath += ".part";
        if (std::filesystem::exists(partPath)) {
            if (!request.overwrite)
                throw std::runtime_error("temporary output already exists: " + partPath.string());
            std::filesystem::remove(partPath);
        }

        const auto* encoder = findVideoEncoder(request.codec);
        if (encoder == nullptr)
            throw std::runtime_error("requested video encoder is unavailable");
        ffmpeg::check(avformat_alloc_output_context2(&format, nullptr, "mp4", request.destination.string().c_str()),
                      "create MP4 output");
        try {
            videoStream = avformat_new_stream(format, encoder);
            if (videoStream == nullptr)
                throw std::bad_alloc();
            videoCodec = avcodec_alloc_context3(encoder);
            if (videoCodec == nullptr)
                throw std::bad_alloc();
            videoCodec->codec_id = videoCodecId(request.codec);
            videoCodec->codec_type = AVMEDIA_TYPE_VIDEO;
            videoCodec->width = static_cast<int>(request.width);
            videoCodec->height = static_cast<int>(request.height);
            videoCodec->pix_fmt = AV_PIX_FMT_YUV420P;
            videoCodec->bit_rate = request.bitrate;
            const auto frameRate = av_d2q(request.fps, 100000);
            videoCodec->time_base = {frameRate.den > 0 ? frameRate.den : 1, frameRate.num > 0 ? frameRate.num : 30};
            videoCodec->framerate = frameRate;
            videoStream->avg_frame_rate = frameRate;
            videoStream->r_frame_rate = frameRate;
            videoCodec->gop_size = std::max(1, static_cast<int>(std::round(request.fps * 2.0)));
            // Keep timestamps in presentation order. This also makes the
            // MP4 duration exactly match encodedFrames / fps on all muxers.
            videoCodec->max_b_frames = 0;
            if ((format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
                videoCodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }
            ffmpeg::check(avcodec_open2(videoCodec, encoder, nullptr), "open video encoder");
            ffmpeg::check(avcodec_parameters_from_context(videoStream->codecpar, videoCodec),
                          "copy video encoder parameters");
            videoStream->time_base = videoCodec->time_base;

            if (request.includeAudio)
                initializeAudio();
            scaler = sws_getContext(static_cast<int>(request.width), static_cast<int>(request.height), AV_PIX_FMT_RGBA,
                                    static_cast<int>(request.width), static_cast<int>(request.height),
                                    AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (scaler == nullptr)
                throw std::runtime_error("create video color converter failed");
            videoFrame = av_frame_alloc();
            packet = av_packet_alloc();
            pendingAudioPacket = av_packet_alloc();
            if (videoFrame == nullptr || packet == nullptr || pendingAudioPacket == nullptr)
                throw std::bad_alloc();
            videoFrame->format = AV_PIX_FMT_YUV420P;
            videoFrame->width = static_cast<int>(request.width);
            videoFrame->height = static_cast<int>(request.height);
            ffmpeg::check(av_frame_get_buffer(videoFrame, 32), "allocate video frame");
            ffmpeg::check(avio_open(&format->pb, partPath.string().c_str(), AVIO_FLAG_WRITE), "open MP4 output");
            ffmpeg::check(avformat_write_header(format, nullptr), "write MP4 header");
            headerWritten = true;
        } catch (...) {
            close(false);
            throw;
        }
    }

    void initializeAudio() {
        const auto* encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (encoder == nullptr)
            throw std::runtime_error("AAC encoder is unavailable");
        audioStream = avformat_new_stream(format, encoder);
        if (audioStream == nullptr)
            throw std::bad_alloc();
        audioCodec = avcodec_alloc_context3(encoder);
        if (audioCodec == nullptr)
            throw std::bad_alloc();
        audioCodec->codec_id = AV_CODEC_ID_AAC;
        audioCodec->codec_type = AVMEDIA_TYPE_AUDIO;
        audioCodec->sample_fmt = AV_SAMPLE_FMT_FLTP;
        audioCodec->sample_rate = 48'000;
        audioCodec->bit_rate = request.audioBitrate;
        audioCodec->time_base = {1, audioCodec->sample_rate};
        av_channel_layout_default(&audioCodec->ch_layout, 2);
        if ((format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
            audioCodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        ffmpeg::check(avcodec_open2(audioCodec, encoder, nullptr), "open AAC encoder");
        ffmpeg::check(avcodec_parameters_from_context(audioStream->codecpar, audioCodec),
                      "copy AAC encoder parameters");
        audioStream->time_base = audioCodec->time_base;
    }

    void ensureResampler(std::uint32_t sampleRate, std::uint32_t channels) {
        if (sampleRate == 0 || channels == 0 || channels > 32) {
            throw std::invalid_argument("invalid input audio format");
        }
        if (resampler != nullptr)
            return;
        AVChannelLayout inputLayout{};
        av_channel_layout_default(&inputLayout, static_cast<int>(channels));
        ffmpeg::check(swr_alloc_set_opts2(&resampler, &audioCodec->ch_layout, AV_SAMPLE_FMT_FLTP,
                                          audioCodec->sample_rate, &inputLayout, AV_SAMPLE_FMT_FLT,
                                          static_cast<int>(sampleRate), 0, nullptr),
                      "create video audio resampler");
        av_channel_layout_uninit(&inputLayout);
        ffmpeg::check(swr_init(resampler), "initialize video audio resampler");
    }

    void encodeVideoFrame(AVFrame* frame) {
        ffmpeg::check(avcodec_send_frame(videoCodec, frame), "submit video frame");
        receiveVideoPackets();
    }

    void receiveVideoPackets() {
        while (true) {
            const int result = avcodec_receive_packet(videoCodec, packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                return;
            ffmpeg::check(result, "receive encoded video packet");
            av_packet_rescale_ts(packet, videoCodec->time_base, videoStream->time_base);
            packet->stream_index = videoStream->index;
            ffmpeg::check(av_interleaved_write_frame(format, packet), "write video packet");
            av_packet_unref(packet);
        }
    }

    void encodeAudioFrame(std::span<const float> left, std::span<const float> right) {
        if (left.empty())
            return;
        auto* frame = av_frame_alloc();
        if (frame == nullptr)
            throw std::bad_alloc();
        try {
            frame->format = audioCodec->sample_fmt;
            frame->sample_rate = audioCodec->sample_rate;
            frame->nb_samples = static_cast<int>(left.size());
            frame->ch_layout = audioCodec->ch_layout;
            ffmpeg::check(av_frame_get_buffer(frame, 0), "allocate audio frame");
            std::memcpy(frame->data[0], left.data(), left.size_bytes());
            std::memcpy(frame->data[1], right.data(), right.size_bytes());
            frame->pts = static_cast<std::int64_t>(audioSamples);
            ffmpeg::check(avcodec_send_frame(audioCodec, frame), "submit audio frame");
            receiveAudioPackets();
            audioSamples += left.size();
        } catch (...) {
            freeFrame(frame);
            throw;
        }
        freeFrame(frame);
    }

    void writePendingAudioPacket() {
        if (pendingAudioPacket == nullptr || pendingAudioPacket->data == nullptr)
            return;
        ffmpeg::check(av_interleaved_write_frame(format, pendingAudioPacket), "write audio packet");
        av_packet_unref(pendingAudioPacket);
    }

    void receiveAudioPackets() {
        while (true) {
            const int result = avcodec_receive_packet(audioCodec, packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                return;
            ffmpeg::check(result, "receive encoded audio packet");
            av_packet_rescale_ts(packet, audioCodec->time_base, audioStream->time_base);
            packet->stream_index = audioStream->index;
            writePendingAudioPacket();
            ffmpeg::check(av_packet_ref(pendingAudioPacket, packet), "retain audio packet");
            av_packet_unref(packet);
        }
    }

    void markAudioPaddingAndWriteFinalPacket() {
        if (pendingAudioPacket == nullptr || pendingAudioPacket->data == nullptr)
            return;
        if (audioSamples > audioTargetSamples) {
            const auto padding =
                std::min<std::uint64_t>(audioSamples - audioTargetSamples, std::numeric_limits<std::uint32_t>::max());
            std::size_t sideDataSize = 0;
            auto* sideData = av_packet_get_side_data(pendingAudioPacket, AV_PKT_DATA_SKIP_SAMPLES, &sideDataSize);
            if (sideData == nullptr || sideDataSize < 10) {
                sideData = av_packet_new_side_data(pendingAudioPacket, AV_PKT_DATA_SKIP_SAMPLES, 10);
                if (sideData == nullptr)
                    throw std::bad_alloc();
                std::memset(sideData, 0, 10);
            }
            sideData[4] = static_cast<std::uint8_t>(padding & 0xffU);
            sideData[5] = static_cast<std::uint8_t>((padding >> 8U) & 0xffU);
            sideData[6] = static_cast<std::uint8_t>((padding >> 16U) & 0xffU);
            sideData[7] = static_cast<std::uint8_t>((padding >> 24U) & 0xffU);
            sideData[9] = 0;
        }
        writePendingAudioPacket();
    }

    void flushAudioResampler() {
        if (resampler == nullptr)
            return;
        const int capacity = 4096;
        std::array<std::vector<float>, 2> converted{std::vector<float>(capacity), std::vector<float>(capacity)};
        while (true) {
            std::uint8_t* output[]{
                reinterpret_cast<std::uint8_t*>(converted[0].data()),
                reinterpret_cast<std::uint8_t*>(converted[1].data()),
            };
            const int count = swr_convert(resampler, output, capacity, nullptr, 0);
            ffmpeg::check(count, "flush video audio resampler");
            if (count == 0)
                break;
            pendingAudio_[0].insert(pendingAudio_[0].end(), converted[0].begin(), converted[0].begin() + count);
            pendingAudio_[1].insert(pendingAudio_[1].end(), converted[1].begin(), converted[1].begin() + count);
            encodePendingAudio(false);
        }
    }

    void encodePendingAudio(bool pad) {
        const auto frameSize = audioCodec->frame_size > 0 ? audioCodec->frame_size : 1024;
        while (pendingAudio_[0].size() >= static_cast<std::size_t>(frameSize) || (pad && !pendingAudio_[0].empty())) {
            const auto count = pad ? std::min<std::size_t>(pendingAudio_[0].size(), static_cast<std::size_t>(frameSize))
                                   : static_cast<std::size_t>(frameSize);
            std::vector<float> left(static_cast<std::size_t>(frameSize));
            std::vector<float> right(static_cast<std::size_t>(frameSize));
            std::copy_n(pendingAudio_[0].begin(), count, left.begin());
            std::copy_n(pendingAudio_[1].begin(), count, right.begin());
            encodeAudioFrame({left.data(), left.size()}, {right.data(), right.size()});
            pendingAudio_[0].erase(pendingAudio_[0].begin(), pendingAudio_[0].begin() + count);
            pendingAudio_[1].erase(pendingAudio_[1].begin(), pendingAudio_[1].begin() + count);
            if (pad)
                break;
        }
    }

    void close(bool commit) noexcept {
        if (format != nullptr && commit && headerWritten)
            av_write_trailer(format);
        if (format != nullptr && format->pb != nullptr)
            avio_closep(&format->pb);
        freeFrame(videoFrame);
        freePacket(packet);
        freePacket(pendingAudioPacket);
        if (resampler != nullptr)
            swr_free(&resampler);
        if (scaler != nullptr) {
            sws_freeContext(scaler);
            scaler = nullptr;
        }
        if (audioCodec != nullptr)
            avcodec_free_context(&audioCodec);
        if (videoCodec != nullptr)
            avcodec_free_context(&videoCodec);
        if (format != nullptr)
            avformat_free_context(format);
        format = nullptr;
        if (!commit && !partPath.empty()) {
            std::error_code error;
            std::filesystem::remove(partPath, error);
        }
    }
#endif
};

bool canExportVideo(VideoCodec codec) noexcept {
#if DAYO_HAS_MEDIA
    return findVideoEncoder(codec) != nullptr;
#else
    static_cast<void>(codec);
    return false;
#endif
}

VideoExporter::VideoExporter(const VideoExportRequest& request) : impl_(std::make_unique<Impl>(request)) {}
VideoExporter::~VideoExporter() = default;
VideoExporter::VideoExporter(VideoExporter&&) noexcept = default;
VideoExporter& VideoExporter::operator=(VideoExporter&&) noexcept = default;

void VideoExporter::writeVideoFrame(const ImageRgba8& image) {
#if DAYO_HAS_MEDIA
    if (impl_->finished)
        throw std::logic_error("video exporter is already finished");
    if (image.width != impl_->request.width || image.height != impl_->request.height ||
        image.pixels.size() != static_cast<std::size_t>(image.width) * image.height * 4U) {
        throw std::invalid_argument("video frame dimensions or pixels do not match export request");
    }
    ffmpeg::check(av_frame_make_writable(impl_->videoFrame), "prepare video frame");
    const std::uint8_t* source[]{image.pixels.data()};
    const int stride[]{static_cast<int>(image.width * 4U)};
    sws_scale(impl_->scaler, source, stride, 0, static_cast<int>(image.height), impl_->videoFrame->data,
              impl_->videoFrame->linesize);
    impl_->videoFrame->pts = static_cast<std::int64_t>(impl_->videoFrames++);
    impl_->encodeVideoFrame(impl_->videoFrame);
#else
    static_cast<void>(image);
    throw std::runtime_error("FFmpeg support was not built");
#endif
}

void VideoExporter::writeAudio(std::span<const float> samples, std::uint32_t sampleRate, std::uint32_t channels) {
#if DAYO_HAS_MEDIA
    if (impl_->finished)
        throw std::logic_error("video exporter is already finished");
    if (!impl_->request.includeAudio || samples.empty())
        return;
    if (channels == 0 || samples.size() % channels != 0) {
        throw std::invalid_argument("interleaved audio samples do not match channel count");
    }
    impl_->ensureResampler(sampleRate, channels);
    const auto inputSamples = samples.size() / channels;
    const auto capacity =
        av_rescale_rnd(swr_get_delay(impl_->resampler, sampleRate) + static_cast<int64_t>(inputSamples),
                       impl_->audioCodec->sample_rate, sampleRate, AV_ROUND_UP);
    std::array<std::vector<float>, 2> converted{std::vector<float>(static_cast<std::size_t>(capacity)),
                                                std::vector<float>(static_cast<std::size_t>(capacity))};
    const std::uint8_t* input[]{reinterpret_cast<const std::uint8_t*>(samples.data())};
    std::uint8_t* output[]{
        reinterpret_cast<std::uint8_t*>(converted[0].data()),
        reinterpret_cast<std::uint8_t*>(converted[1].data()),
    };
    const int count = swr_convert(impl_->resampler, output, capacity, input, static_cast<int>(inputSamples));
    ffmpeg::check(count, "resample video audio");
    impl_->pendingAudio_[0].insert(impl_->pendingAudio_[0].end(), converted[0].begin(), converted[0].begin() + count);
    impl_->pendingAudio_[1].insert(impl_->pendingAudio_[1].end(), converted[1].begin(), converted[1].begin() + count);
    impl_->encodePendingAudio(false);
#else
    static_cast<void>(samples);
    static_cast<void>(sampleRate);
    static_cast<void>(channels);
    throw std::runtime_error("FFmpeg support was not built");
#endif
}

VideoExportResult VideoExporter::finish() {
#if DAYO_HAS_MEDIA
    if (impl_->finished)
        throw std::logic_error("video exporter is already finished");
    if (impl_->videoFrames == 0)
        throw std::runtime_error("video export contains no frames");
    if (impl_->request.includeAudio) {
        impl_->flushAudioResampler();
        impl_->encodePendingAudio(true);
        ffmpeg::check(avcodec_send_frame(impl_->audioCodec, nullptr), "flush AAC encoder");
        impl_->receiveAudioPackets();
        const AVRational audioTimeBase{1, impl_->audioCodec->sample_rate};
        impl_->audioTargetSamples =
            av_rescale_q(static_cast<std::int64_t>(impl_->videoFrames), impl_->videoCodec->time_base, audioTimeBase);
        impl_->markAudioPaddingAndWriteFinalPacket();
    }
    ffmpeg::check(avcodec_send_frame(impl_->videoCodec, nullptr), "flush video encoder");
    impl_->receiveVideoPackets();
    impl_->videoStream->duration = av_rescale_q(static_cast<std::int64_t>(impl_->videoFrames),
                                                impl_->videoCodec->time_base, impl_->videoStream->time_base);
    if (impl_->audioStream != nullptr) {
        impl_->audioStream->duration = av_rescale_q(static_cast<std::int64_t>(impl_->videoFrames),
                                                    impl_->videoCodec->time_base, impl_->audioStream->time_base);
    }
    ffmpeg::check(av_write_trailer(impl_->format), "write MP4 trailer");
    impl_->headerWritten = false;
    if (impl_->format->pb != nullptr)
        ffmpeg::check(avio_closep(&impl_->format->pb), "close MP4 output");
    impl_->finished = true;
    impl_->close(true);
    std::error_code error;
    std::filesystem::rename(impl_->partPath, impl_->request.destination, error);
    if (error) {
        impl_->close(false);
        throw std::runtime_error("commit video output failed: " + error.message());
    }
    return {impl_->request.destination, impl_->videoFrames,
            static_cast<double>(impl_->videoFrames) / impl_->request.fps};
#else
    throw std::runtime_error("FFmpeg support was not built");
#endif
}

} // namespace dayo::core
