// Link the Release media/ffmpeg_utils objects with --wrap (see binary-audit.md).
// Wrappers count calls made directly by those objects, not FFmpeg internals.
#include "core/media.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

int __real_av_seek_frame(AVFormatContext*, int, std::int64_t, int);
int __real_avcodec_send_packet(AVCodecContext*, const AVPacket*);
SwsContext* __real_sws_getContext(int, int, AVPixelFormat, int, int, AVPixelFormat, int, SwsFilter*, SwsFilter*,
                                  const double*);
}

namespace {
std::uint64_t seeks{}, packets{}, scalers{};
}

extern "C" int __wrap_av_seek_frame(AVFormatContext* context, int stream, std::int64_t timestamp, int flags) {
    ++seeks;
    return __real_av_seek_frame(context, stream, timestamp, flags);
}
extern "C" int __wrap_avcodec_send_packet(AVCodecContext* context, const AVPacket* packet) {
    ++packets;
    return __real_avcodec_send_packet(context, packet);
}
extern "C" SwsContext* __wrap_sws_getContext(int sw, int sh, AVPixelFormat sf, int dw, int dh, AVPixelFormat df,
                                             int flags, SwsFilter* src, SwsFilter* dst, const double* param) {
    ++scalers;
    return __real_sws_getContext(sw, sh, sf, dw, dh, df, flags, src, dst, param);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: media_decode_probe video.mp4\n";
        return 1;
    }
    try {
        dayo::core::MediaFile media(argv[1], dayo::core::MediaOpenMode::videoOnly);
        const auto warmup = media.decodeVideoFrame(0.0);
        seeks = packets = scalers = 0;
        std::uint64_t checksum = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int frame = 0; frame < 60; ++frame) {
            const auto image = media.decodeVideoFrame(static_cast<double>(frame) / 30.0);
            if (image.width != warmup.width || image.height != warmup.height || image.pixels.empty())
                throw std::runtime_error("invalid decoded dimensions");
            checksum += image.pixels.front();
        }
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::cout << "{\"frames\":60,\"seeks\":" << seeks << ",\"packets\":" << packets << ",\"scalers\":" << scalers
                  << ",\"milliseconds\":" << ms << ",\"checksum\":" << checksum << "}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
