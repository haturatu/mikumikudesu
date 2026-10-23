#include "core/media.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
int __real_av_seek_frame(AVFormatContext*, int, std::int64_t, int);
int __real_avcodec_send_packet(AVCodecContext*, const AVPacket*);
}

namespace {
std::size_t seeks{}, packets{};
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace

extern "C" int __wrap_av_seek_frame(AVFormatContext* context, int stream, std::int64_t timestamp, int flags) {
    ++seeks;
    return __real_av_seek_frame(context, stream, timestamp, flags);
}
extern "C" int __wrap_avcodec_send_packet(AVCodecContext* context, const AVPacket* packet) {
    ++packets;
    return __real_avcodec_send_packet(context, packet);
}

int main(int argc, char** argv) {
    try {
        require(argc == 4, "usage: media_decode_tests video raw.rgba pts.txt");
        std::ifstream timestamps(argv[3]);
        const bool changingDimensions = std::string_view(argv[2]) == "-";
        std::ifstream raw(argv[2], std::ios::binary);
        std::vector<double> times;
        std::vector<dayo::core::ImageRgba8> expected;
        double time;
        std::uint32_t width, height;
        while (timestamps >> time >> width >> height) {
            dayo::core::ImageRgba8 image;
            if (changingDimensions) {
                // FFmpeg's rawvideo muxer fixes its output dimensions. Use an
                // independent seek for each variable-size frame instead.
                dayo::core::MediaFile fresh(argv[1]);
                image = fresh.decodeVideoFrame(time);
                require(image.width == width && image.height == height, "seeked dimensions must match ffprobe");
            } else {
                image = {width, height, {}};
                image.pixels.resize(static_cast<std::size_t>(width) * height * 4U);
                raw.read(reinterpret_cast<char*>(image.pixels.data()),
                         static_cast<std::streamsize>(image.pixels.size()));
                require(raw.good(), "truncated raw reference");
            }
            times.push_back(time);
            expected.push_back(std::move(image));
        }
        require(times.size() > 4, "missing frame references");
        const auto matches = [&](const auto& image, std::size_t index) {
            require(image.width == expected[index].width && image.height == expected[index].height,
                    "decoded frame dimensions differ from reference");
            require(image.pixels == expected[index].pixels, "decoded RGBA differs from FFmpeg reference");
        };
        dayo::core::MediaFile media(argv[1]);
        seeks = packets = 0;
        for (std::size_t i = 0; i < times.size(); ++i)
            matches(media.decodeVideoFrame(times[i]), i);
        require(seeks == 1, "continuous playback must seek only once");
        require(packets <= times.size() + 4, "continuous playback must not repeatedly decode a GOP");
        std::cout << "sequential frames=" << times.size() << " seeks=" << seeks << " packets=" << packets << '\n';

        const auto endSeeks = seeks;
        matches(media.decodeVideoFrame(times.back()), times.size() - 1);
        require(seeks == endSeeks, "repeated time must reuse the retained frame");
        matches(media.decodeVideoFrame(times[0]), 0);
        require(seeks == endSeeks + 1, "backward requests must seek");
        matches(media.decodeVideoFrame((times[0] + times[1]) / 2.0), 1);
        matches(media.decodeVideoFrame((times[0] + times[1]) / 2.0), 1);
        matches(media.decodeVideoFrame(times[1]), 1);
        const auto jumpSeeks = seeks;
        matches(media.decodeVideoFrame(times.back()), times.size() - 1);
        require(seeks == jumpSeeks + 1, "large forward jumps must seek instead of decoding the whole interval");

        bool eof = false;
        try {
            static_cast<void>(media.decodeVideoFrame(times.back() + 2.0));
        } catch (const std::runtime_error&) {
            eof = true;
        }
        require(eof, "a request beyond EOF must fail");
        matches(media.decodeVideoFrame(times[0]), 0);
        for (double invalid : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::max()}) {
            bool rejected = false;
            try {
                static_cast<void>(media.decodeVideoFrame(invalid));
            } catch (const std::logic_error&) {
                rejected = true;
            }
            require(rejected, "invalid video time must be rejected before converting to a timestamp");
        }
        matches(media.decodeVideoFrame(times[1]), 1);
        auto moved = std::move(media);
        const auto moveSeeks = seeks;
        matches(moved.decodeVideoFrame(times[2]), 2);
        require(seeks == moveSeeks, "moving a MediaFile must preserve pending decoder output");
        dayo::core::MediaFile assigned(argv[1]);
        assigned = std::move(moved);
        matches(assigned.decodeVideoFrame(times[3]), 3);
        if (assigned.info().hasAudio) {
            const auto audio = assigned.decodeAudio();
            require(!audio.samples.empty(), "shared audio decoder must still produce samples");
            const auto audioSeeks = seeks;
            matches(assigned.decodeVideoFrame(times[3]), 3);
            require(seeks == audioSeeks + 1, "audio demuxing must invalidate the video packet position");
            matches(assigned.decodeVideoFrame(times[4]), 4);
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
