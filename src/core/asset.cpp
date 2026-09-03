#include "core/asset.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace dayo::core {
namespace {

template <std::size_t N> bool contains(const std::array<std::string_view, N>& values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

AssetKind classifyAsset(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (extension == ".pmx")
        return AssetKind::pmx;
    if (extension == ".vmd")
        return AssetKind::vmd;
    if (extension == ".vpd")
        return AssetKind::vpd;
    if (extension == ".vmdayo")
        return AssetKind::vmdayo;
    if (extension == ".dayo")
        return AssetKind::project;
    if (extension == ".fxdayo")
        return AssetKind::effect;

    constexpr std::array<std::string_view, 7> images{".bmp", ".jpg", ".jpeg", ".png", ".hdr", ".tga", ".dds"};
    constexpr std::array<std::string_view, 5> audio{".wav", ".mp3", ".m4a", ".flac", ".ogg"};
    constexpr std::array<std::string_view, 5> video{".mp4", ".avi", ".mkv", ".mov", ".webm"};
    if (contains(images, extension))
        return AssetKind::image;
    if (contains(audio, extension))
        return AssetKind::audio;
    if (contains(video, extension))
        return AssetKind::video;
    return AssetKind::unknown;
}

std::string_view toString(AssetKind kind) noexcept {
    switch (kind) {
    case AssetKind::pmx:
        return "PMX model";
    case AssetKind::vmd:
        return "VMD motion";
    case AssetKind::vpd:
        return "VPD pose";
    case AssetKind::vmdayo:
        return "VMdayo motion";
    case AssetKind::image:
        return "image";
    case AssetKind::audio:
        return "audio";
    case AssetKind::video:
        return "video";
    case AssetKind::project:
        return "project";
    case AssetKind::effect:
        return "effect";
    case AssetKind::unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace dayo::core
