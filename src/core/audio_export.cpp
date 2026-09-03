#include "core/audio_export.hpp"

#include "core/ffmpeg_utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if DAYO_HAS_MEDIA
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#endif

namespace dayo::core {
namespace {

void validateRequest(const AudioExportRequest& request) {
    if (request.source.empty())
        throw std::invalid_argument("audio export source is empty");
    if (request.destination.empty())
        throw std::invalid_argument("audio export destination is empty");
    if (request.bitrate == 0)
        throw std::invalid_argument("audio export bitrate must be positive");
    if (request.sampleRate == 0)
        throw std::invalid_argument("audio export sample rate must be positive");
    if (request.channels == 0 || request.channels > 8) {
        throw std::invalid_argument("audio export channels must be between 1 and 8");
    }
    if (request.startSeconds && (!std::isfinite(*request.startSeconds) || *request.startSeconds < 0.0)) {
        throw std::invalid_argument("audio export start time must be a finite non-negative number");
    }
    if (request.endSeconds && (!std::isfinite(*request.endSeconds) || *request.endSeconds < 0.0)) {
        throw std::invalid_argument("audio export end time must be a finite non-negative number");
    }
    if (request.startSeconds && request.endSeconds && *request.endSeconds <= *request.startSeconds) {
        throw std::invalid_argument("audio export end time must be greater than start time");
    }
}

#if DAYO_HAS_MEDIA
struct ExportContext {
    AVFormatContext* input{};
    AVCodecContext* decoder{};
    AVFormatContext* output{};
    AVCodecContext* encoder{};
    SwrContext* resampler{};
    AVAudioFifo* fifo{};
    AVPacket* packet{};
    AVPacket* encodedPacket{};

    ~ExportContext() {
        av_packet_free(&packet);
        av_packet_free(&encodedPacket);
        av_audio_fifo_free(fifo);
        swr_free(&resampler);
        avcodec_free_context(&decoder);
        avcodec_free_context(&encoder);
        avformat_close_input(&input);
        if (output != nullptr) {
            if (output->pb != nullptr)
                avio_closep(&output->pb);
            avformat_free_context(output);
        }
    }
};

AVCodecContext* openDecoder(AVFormatContext* format, int streamIndex) {
    const auto* codec = avcodec_find_decoder(format->streams[streamIndex]->codecpar->codec_id);
    if (codec == nullptr)
        throw std::runtime_error("FFmpeg decoder is unavailable");
    auto* context = avcodec_alloc_context3(codec);
    if (context == nullptr)
        throw std::bad_alloc();
    try {
        ffmpeg::check(avcodec_parameters_to_context(context, format->streams[streamIndex]->codecpar),
                      "copy audio decoder parameters");
        ffmpeg::check(avcodec_open2(context, codec, nullptr), "open audio decoder");
    } catch (...) {
        avcodec_free_context(&context);
        throw;
    }
    return context;
}

void checkCancelled(const std::stop_token& stopToken) {
    if (stopToken.stop_requested())
        throw std::runtime_error("audio export cancelled");
}

std::filesystem::path partPath(const std::filesystem::path& destination) {
    auto part = destination;
    part += ".part";
    return part;
}

AudioExportResult exportM4aWithFfmpeg(const AudioExportRequest& request, const AudioExportProgressCallback& progress,
                                      const std::stop_token& stopToken) {
    const auto temporaryOutput = partPath(request.destination);
    std::error_code filesystemError;
    if (std::filesystem::exists(request.destination, filesystemError) && !request.overwrite) {
        throw std::runtime_error("audio export destination already exists: " + request.destination.string());
    }
    if (filesystemError) {
        throw std::runtime_error("check audio export destination: " + filesystemError.message());
    }
    if (const auto parent = request.destination.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, filesystemError);
        if (filesystemError) {
            throw std::runtime_error("create audio export directory: " + filesystemError.message());
        }
    }
    std::filesystem::remove(temporaryOutput, filesystemError);
    if (filesystemError) {
        throw std::runtime_error("remove incomplete audio export: " + filesystemError.message());
    }

    try {
        checkCancelled(stopToken);
        ExportContext context;
        const auto sourceName = request.source.string();
        ffmpeg::check(avformat_open_input(&context.input, sourceName.c_str(), nullptr, nullptr),
                      "open audio export source");
        ffmpeg::check(avformat_find_stream_info(context.input, nullptr), "read audio export streams");
        const int audioStreamIndex = av_find_best_stream(context.input, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (audioStreamIndex < 0)
            throw std::runtime_error("audio export source has no audio stream");
        context.decoder = openDecoder(context.input, audioStreamIndex);
        const auto* inputStream = context.input->streams[audioStreamIndex];
        if (context.decoder->sample_rate <= 0)
            throw std::runtime_error("audio source has no sample rate");
        const int inputChannels = context.decoder->ch_layout.nb_channels;
        if (inputChannels <= 0)
            throw std::runtime_error("audio source has no channel layout");

        const auto* encoderCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (encoderCodec == nullptr)
            throw std::runtime_error("AAC encoder is unavailable");
        context.encoder = avcodec_alloc_context3(encoderCodec);
        if (context.encoder == nullptr)
            throw std::bad_alloc();
        context.encoder->bit_rate = static_cast<std::int64_t>(request.bitrate);
        context.encoder->sample_rate = static_cast<int>(request.sampleRate);
        // The native AAC encoder accepts planar float samples on current FFmpeg versions.
        context.encoder->sample_fmt = AV_SAMPLE_FMT_FLTP;
        context.encoder->time_base = AVRational{1, static_cast<int>(request.sampleRate)};
        av_channel_layout_default(&context.encoder->ch_layout, static_cast<int>(request.channels));

        ffmpeg::check(
            avformat_alloc_output_context2(&context.output, nullptr, "ipod", request.destination.string().c_str()),
            "create M4A output");
        if (context.output == nullptr)
            throw std::runtime_error("create M4A output: unknown format");
        if ((context.output->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
            context.encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        ffmpeg::check(avcodec_open2(context.encoder, encoderCodec, nullptr), "open AAC encoder");

        auto* outputStream = avformat_new_stream(context.output, nullptr);
        if (outputStream == nullptr)
            throw std::bad_alloc();
        outputStream->time_base = context.encoder->time_base;
        ffmpeg::check(avcodec_parameters_from_context(outputStream->codecpar, context.encoder),
                      "copy AAC stream parameters");
        outputStream->codecpar->codec_tag = 0;
        if ((context.output->oformat->flags & AVFMT_NOFILE) == 0) {
            ffmpeg::check(avio_open(&context.output->pb, temporaryOutput.string().c_str(), AVIO_FLAG_WRITE),
                          "open M4A output");
        }
        ffmpeg::check(avformat_write_header(context.output, nullptr), "write M4A header");

        AVChannelLayout inputLayout{};
        if (context.decoder->ch_layout.nb_channels > 0) {
            ffmpeg::check(av_channel_layout_copy(&inputLayout, &context.decoder->ch_layout),
                          "copy input channel layout");
        } else {
            av_channel_layout_default(&inputLayout, inputChannels);
        }
        ffmpeg::check(swr_alloc_set_opts2(&context.resampler, &context.encoder->ch_layout, context.encoder->sample_fmt,
                                          context.encoder->sample_rate, &inputLayout, context.decoder->sample_fmt,
                                          context.decoder->sample_rate, 0, nullptr),
                      "create audio export resampler");
        av_channel_layout_uninit(&inputLayout);
        ffmpeg::check(swr_init(context.resampler), "initialize audio export resampler");
        context.fifo = av_audio_fifo_alloc(context.encoder->sample_fmt, static_cast<int>(request.channels), 4096);
        if (context.fifo == nullptr)
            throw std::bad_alloc();
        context.packet = av_packet_alloc();
        context.encodedPacket = av_packet_alloc();
        if (context.packet == nullptr || context.encodedPacket == nullptr)
            throw std::bad_alloc();

        const double inputTimeBase = av_q2d(inputStream->time_base);
        const double sourceDuration =
            inputStream->duration > 0
                ? static_cast<double>(inputStream->duration) * inputTimeBase
                : (context.input->duration > 0 ? static_cast<double>(context.input->duration) / AV_TIME_BASE : 0.0);
        const double startSeconds = request.startSeconds.value_or(0.0);
        const double endSeconds = request.endSeconds.value_or(sourceDuration);
        if (sourceDuration > 0.0 && startSeconds >= sourceDuration) {
            throw std::invalid_argument("audio export start time is outside the source duration");
        }
        if (request.endSeconds && sourceDuration > 0.0 && *request.endSeconds > sourceDuration + 1e-6) {
            throw std::invalid_argument("audio export end time is outside the source duration");
        }
        const double totalSeconds = endSeconds > startSeconds ? endSeconds - startSeconds : 0.0;
        AudioExportProgress currentProgress{0.0, totalSeconds};
        if (progress)
            progress(currentProgress);

        std::uint64_t encodedSamples = 0;
        std::uint64_t selectedInputSamples = 0;
        double sourceCursor = 0.0;
        bool reachedEnd = false;
        const int frameSize = context.encoder->frame_size > 0 ? context.encoder->frame_size : 1024;

        auto receivePackets = [&] {
            while (true) {
                const int result = avcodec_receive_packet(context.encoder, context.encodedPacket);
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                    return;
                ffmpeg::check(result, "receive AAC packet");
                context.encodedPacket->stream_index = outputStream->index;
                av_packet_rescale_ts(context.encodedPacket, context.encoder->time_base, outputStream->time_base);
                ffmpeg::check(av_interleaved_write_frame(context.output, context.encodedPacket), "write AAC packet");
                av_packet_unref(context.encodedPacket);
            }
        };

        auto encodeSamples = [&](int sampleCount, bool pad) {
            if (sampleCount <= 0)
                return;
            auto* frame = av_frame_alloc();
            if (frame == nullptr)
                throw std::bad_alloc();
            try {
                frame->format = context.encoder->sample_fmt;
                frame->sample_rate = context.encoder->sample_rate;
                frame->nb_samples = frameSize;
                ffmpeg::check(av_channel_layout_copy(&frame->ch_layout, &context.encoder->ch_layout),
                              "copy AAC frame channel layout");
                ffmpeg::check(av_frame_get_buffer(frame, 0), "allocate AAC frame");
                ffmpeg::check(
                    av_audio_fifo_read(context.fifo, reinterpret_cast<void**>(frame->extended_data), sampleCount),
                    "read audio export samples");
                if (pad && sampleCount < frameSize) {
                    av_samples_set_silence(frame->extended_data, sampleCount, frameSize - sampleCount,
                                           static_cast<int>(request.channels), context.encoder->sample_fmt);
                }
                frame->pts = static_cast<std::int64_t>(encodedSamples);
                checkCancelled(stopToken);
                ffmpeg::check(avcodec_send_frame(context.encoder, frame), "submit AAC frame");
                receivePackets();
                encodedSamples += static_cast<std::uint64_t>(frameSize);
                av_frame_free(&frame);
            } catch (...) {
                av_frame_free(&frame);
                throw;
            }
        };

        auto encodeAvailable = [&](bool partial) {
            while (av_audio_fifo_size(context.fifo) >= frameSize || (partial && av_audio_fifo_size(context.fifo) > 0)) {
                const int available = av_audio_fifo_size(context.fifo);
                encodeSamples(std::min(available, frameSize), partial && available < frameSize);
            }
        };

        auto consumeDecodedFrame = [&](AVFrame* frame) {
            checkCancelled(stopToken);
            const auto sampleFormat = static_cast<AVSampleFormat>(frame->format);
            const int bytesPerSample = av_get_bytes_per_sample(sampleFormat);
            if (bytesPerSample <= 0 || frame->nb_samples <= 0)
                return;
            const double frameStart = frame->best_effort_timestamp == AV_NOPTS_VALUE
                                          ? sourceCursor
                                          : static_cast<double>(frame->best_effort_timestamp) * inputTimeBase;
            const double frameEnd = frameStart + static_cast<double>(frame->nb_samples) / context.decoder->sample_rate;
            sourceCursor = frameEnd;
            if (request.endSeconds && frameStart >= *request.endSeconds) {
                reachedEnd = true;
                return;
            }
            const double clipEnd = std::min(frameEnd, endSeconds > startSeconds ? endSeconds : frameEnd);
            const double clipStart = std::max(frameStart, startSeconds);
            if (clipEnd <= clipStart)
                return;
            const int skipSamples =
                std::clamp(static_cast<int>(std::floor((clipStart - frameStart) * context.decoder->sample_rate + 1e-6)),
                           0, frame->nb_samples);
            const int clipSamples =
                std::clamp(static_cast<int>(std::ceil((clipEnd - (frameStart + static_cast<double>(skipSamples) /
                                                                                   context.decoder->sample_rate)) *
                                                          context.decoder->sample_rate -
                                                      1e-6)),
                           0, frame->nb_samples - skipSamples);
            if (clipSamples <= 0)
                return;

            std::vector<const std::uint8_t*> inputPointers(av_sample_fmt_is_planar(sampleFormat) ? inputChannels : 1);
            if (inputPointers.empty())
                inputPointers.resize(1);
            if (av_sample_fmt_is_planar(sampleFormat)) {
                for (std::size_t channel = 0; channel < inputPointers.size(); ++channel) {
                    inputPointers[channel] =
                        frame->extended_data[channel] + static_cast<std::ptrdiff_t>(skipSamples * bytesPerSample);
                }
            } else {
                inputPointers[0] =
                    frame->extended_data[0] + static_cast<std::ptrdiff_t>(skipSamples * bytesPerSample * inputChannels);
            }
            const auto outputCapacity =
                av_rescale_rnd(swr_get_delay(context.resampler, context.decoder->sample_rate) + clipSamples,
                               context.encoder->sample_rate, context.decoder->sample_rate, AV_ROUND_UP);
            if (outputCapacity <= 0 || outputCapacity > std::numeric_limits<int>::max()) {
                throw std::runtime_error("audio export frame is too large");
            }
            auto* converted = av_frame_alloc();
            if (converted == nullptr)
                throw std::bad_alloc();
            try {
                converted->format = context.encoder->sample_fmt;
                converted->sample_rate = context.encoder->sample_rate;
                converted->nb_samples = static_cast<int>(outputCapacity);
                ffmpeg::check(av_channel_layout_copy(&converted->ch_layout, &context.encoder->ch_layout),
                              "copy resampled channel layout");
                ffmpeg::check(av_frame_get_buffer(converted, 0), "allocate resampled audio frame");
                const int convertedSamples = swr_convert(context.resampler, converted->extended_data,
                                                         converted->nb_samples, inputPointers.data(), clipSamples);
                ffmpeg::check(convertedSamples, "resample audio for export");
                if (convertedSamples > 0) {
                    ffmpeg::check(
                        av_audio_fifo_realloc(context.fifo, av_audio_fifo_size(context.fifo) + convertedSamples),
                        "grow audio export buffer");
                    ffmpeg::check(av_audio_fifo_write(context.fifo, reinterpret_cast<void**>(converted->extended_data),
                                                      convertedSamples),
                                  "buffer resampled audio");
                    encodeAvailable(false);
                }
                selectedInputSamples += static_cast<std::uint64_t>(clipSamples);
                currentProgress.processedSeconds = std::max(
                    currentProgress.processedSeconds, std::min(totalSeconds, std::max(0.0, clipEnd - startSeconds)));
                if (progress)
                    progress(currentProgress);
                av_frame_free(&converted);
            } catch (...) {
                av_frame_free(&converted);
                throw;
            }
        };

        auto* decodedFrame = av_frame_alloc();
        if (decodedFrame == nullptr)
            throw std::bad_alloc();
        auto receiveDecodedFrames = [&] {
            while (true) {
                const int result = avcodec_receive_frame(context.decoder, decodedFrame);
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                    return;
                ffmpeg::check(result, "decode audio export frame");
                consumeDecodedFrame(decodedFrame);
                av_frame_unref(decodedFrame);
                if (reachedEnd)
                    return;
            }
        };
        try {
            while (!reachedEnd) {
                checkCancelled(stopToken);
                const int result = av_read_frame(context.input, context.packet);
                if (result == AVERROR_EOF)
                    break;
                ffmpeg::check(result, "read audio export packet");
                if (context.packet->stream_index == audioStreamIndex) {
                    ffmpeg::check(avcodec_send_packet(context.decoder, context.packet), "submit audio export packet");
                    receiveDecodedFrames();
                }
                av_packet_unref(context.packet);
            }
            if (!reachedEnd) {
                ffmpeg::check(avcodec_send_packet(context.decoder, nullptr), "flush audio export decoder");
                receiveDecodedFrames();
            }
            av_frame_free(&decodedFrame);
        } catch (...) {
            av_frame_free(&decodedFrame);
            throw;
        }

        while (true) {
            checkCancelled(stopToken);
            const auto outputCapacity = swr_get_out_samples(context.resampler, 0);
            if (outputCapacity <= 0)
                break;
            auto* converted = av_frame_alloc();
            if (converted == nullptr)
                throw std::bad_alloc();
            try {
                converted->format = context.encoder->sample_fmt;
                converted->sample_rate = context.encoder->sample_rate;
                converted->nb_samples = outputCapacity;
                ffmpeg::check(av_channel_layout_copy(&converted->ch_layout, &context.encoder->ch_layout),
                              "copy flushed channel layout");
                ffmpeg::check(av_frame_get_buffer(converted, 0), "allocate flushed audio frame");
                const int convertedSamples =
                    swr_convert(context.resampler, converted->extended_data, outputCapacity, nullptr, 0);
                ffmpeg::check(convertedSamples, "flush audio export resampler");
                if (convertedSamples <= 0) {
                    av_frame_free(&converted);
                    break;
                }
                ffmpeg::check(av_audio_fifo_realloc(context.fifo, av_audio_fifo_size(context.fifo) + convertedSamples),
                              "grow flushed audio buffer");
                ffmpeg::check(av_audio_fifo_write(context.fifo, reinterpret_cast<void**>(converted->extended_data),
                                                  convertedSamples),
                              "buffer flushed audio");
                encodeAvailable(false);
                av_frame_free(&converted);
            } catch (...) {
                av_frame_free(&converted);
                throw;
            }
        }
        if (selectedInputSamples == 0 || (av_audio_fifo_size(context.fifo) == 0 && encodedSamples == 0)) {
            throw std::runtime_error("audio export range contains no samples");
        }
        encodeAvailable(true);
        checkCancelled(stopToken);
        ffmpeg::check(avcodec_send_frame(context.encoder, nullptr), "flush AAC encoder");
        receivePackets();
        ffmpeg::check(av_write_trailer(context.output), "write M4A trailer");

        currentProgress.processedSeconds = totalSeconds > 0.0 ? totalSeconds : currentProgress.processedSeconds;
        if (progress)
            progress(currentProgress);
        const auto durationSeconds = static_cast<double>(encodedSamples) / context.encoder->sample_rate;
        return {request.destination, durationSeconds, encodedSamples};
    } catch (...) {
        std::filesystem::remove(temporaryOutput, filesystemError);
        throw;
    }

    // The return above is reached after the output context has been closed by its owner.
}
#endif

} // namespace

bool canExportM4a() noexcept {
#if DAYO_HAS_MEDIA
    return avcodec_find_encoder(AV_CODEC_ID_AAC) != nullptr;
#else
    return false;
#endif
}

AudioExportResult exportM4a(const AudioExportRequest& request, AudioExportProgressCallback progress,
                            std::stop_token stopToken) {
    validateRequest(request);
#if DAYO_HAS_MEDIA
    if (!canExportM4a())
        throw std::runtime_error("AAC encoder is unavailable");
    const auto result = exportM4aWithFfmpeg(request, progress, stopToken);
    std::error_code filesystemError;
    std::filesystem::rename(partPath(request.destination), request.destination, filesystemError);
    if (filesystemError) {
        const auto renameError = filesystemError;
        std::error_code cleanupError;
        std::filesystem::remove(partPath(request.destination), cleanupError);
        throw std::runtime_error("commit M4A output: " + renameError.message());
    }
    return result;
#else
    static_cast<void>(progress);
    static_cast<void>(stopToken);
    throw std::runtime_error("FFmpeg support was not built");
#endif
}

} // namespace dayo::core
