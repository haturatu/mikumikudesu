#pragma once

#include <filesystem>
#include <string_view>

namespace dayo::core {

enum class AssetKind {
    pmx,
    vmd,
    vpd,
    vmdayo,
    image,
    audio,
    video,
    project,
    effect,
    unknown,
};

[[nodiscard]] AssetKind classifyAsset(const std::filesystem::path& path);
[[nodiscard]] std::string_view toString(AssetKind kind) noexcept;

} // namespace dayo::core
