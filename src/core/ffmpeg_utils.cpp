#include "core/ffmpeg_utils.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>

#if DAYO_HAS_MEDIA
#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif
extern "C" {
#include <libavutil/avutil.h>
}
#endif

namespace dayo::core::ffmpeg {

std::string errorString(int error) {
#if DAYO_HAS_MEDIA
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(error, buffer.data(), buffer.size());
    return buffer.data();
#else
    static_cast<void>(error);
    return "FFmpeg support was not built";
#endif
}

void check(int result, std::string_view operation) {
    if (result < 0)
        throw std::runtime_error(std::string(operation) + ": " + errorString(result));
}

} // namespace dayo::core::ffmpeg
