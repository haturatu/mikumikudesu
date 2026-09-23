#include "core/asset.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <system_error>

namespace dayo::core {
namespace {

template <std::size_t N> bool contains(const std::array<std::string_view, N>& values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

} // namespace

AssetKind classifyAsset(const std::filesystem::path& path) {
    const auto extension = lowercase(path.extension().string());

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

std::optional<std::filesystem::path> findAssociatedEffect(const std::filesystem::path& modelPath) {
    const auto directory = modelPath.parent_path().empty() ? std::filesystem::path{"."} : modelPath.parent_path();
    auto conventionalPath = modelPath;
    conventionalPath.replace_extension(".fxdayo");
    std::error_code error;
    if (std::filesystem::is_regular_file(conventionalPath, error) && !error)
        return conventionalPath;

    error.clear();
    std::filesystem::directory_iterator iterator(directory, error);
    const std::filesystem::directory_iterator end;
    if (error)
        return std::nullopt;
    const auto modelStem = lowercase(modelPath.stem().string());
    for (; iterator != end; iterator.increment(error)) {
        if (error)
            break;
        const auto& candidate = *iterator;
        if (lowercase(candidate.path().extension().string()) != ".fxdayo" ||
            lowercase(candidate.path().stem().string()) != modelStem)
            continue;
        error.clear();
        if (candidate.is_regular_file(error) && !error)
            return candidate.path();
    }
    return std::nullopt;
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
