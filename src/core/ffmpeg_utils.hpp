#pragma once

#include <string>
#include <string_view>

namespace dayo::core::ffmpeg {

[[nodiscard]] std::string errorString(int error);
void check(int result, std::string_view operation);

} // namespace dayo::core::ffmpeg
