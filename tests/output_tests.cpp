#include "core/output.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

bool check(bool value, std::string_view message) {
    if (!value)
        std::cerr << "FAIL: " << message << '\n';
    return value;
}

} // namespace

int main() {
    const auto path = std::filesystem::temp_directory_path() / "mikumikudesu-output-test.exr";
    std::error_code cleanupError;
    std::filesystem::remove(path, cleanupError);
    const dayo::core::ImageRgba8 image{2, 1, std::vector<std::uint8_t>{255, 0, 0, 255, 0, 128, 255, 255}};
    bool ok = true;
    try {
        dayo::core::writeFrame(path, image, dayo::core::OutputFormat::exr);
#if DAYO_HAS_OPENEXR
        std::ifstream input(path, std::ios::binary);
        std::array<std::uint8_t, 4> magic{};
        input.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
        ok &= check(input.good() && magic == std::array<std::uint8_t, 4>{0x76, 0x2f, 0x31, 0x01},
                    "OpenEXR output has the expected file signature");
#else
        ok &= check(false, "EXR output must be rejected when OpenEXR is unavailable");
#endif
    } catch (const std::exception& exception) {
#if DAYO_HAS_OPENEXR
        std::cerr << "EXR encoding failed: " << exception.what() << '\n';
        ok = false;
#else
        ok &= check(std::string_view(exception.what()).find("OpenEXR") != std::string_view::npos,
                    "EXR failure explains the missing encoder");
#endif
    }
    std::filesystem::remove(path, cleanupError);
    return ok ? 0 : 1;
}
