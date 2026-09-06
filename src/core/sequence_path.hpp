#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace dayo::core {

// Bounded movie-sequence path spec: prefix + zero-padded frame + extension.
// Example: {prefix="frame_", start=3, digits=6, extension=".ppm"}.
struct SequencePathSpec {
    std::string prefix{"frame_"};
    std::uint32_t start{};
    std::uint32_t digits{6};
    std::string extension{".ppm"};
};

[[nodiscard]] std::optional<SequencePathSpec> parseSequencePath(const std::filesystem::path& path) noexcept;
[[nodiscard]] std::filesystem::path formatSequencePath(const std::filesystem::path& directory,
                                                       const SequencePathSpec& spec, std::uint32_t frame);
[[nodiscard]] std::filesystem::path formatSequencePath(const SequencePathSpec& spec, std::uint32_t frame);

} // namespace dayo::core
