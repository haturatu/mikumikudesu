#pragma once

#include "core/motion.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <map>
#include <string>
#include <vector>

namespace dayo::core {

// MikuMikuDayo 1.30 VMdayo v3 document. Unknown data is retained in opaque so
// a file that cannot be decoded is never destructively rewritten.
struct VmdayoDocument {
    int version { 3 };
    std::string modelName;
    MotionDocument motion;
    std::vector<std::uint8_t> opaque;
    int type {};
    std::map<std::int32_t, std::string> modelDictionary;
    std::map<std::string, std::string> metadata;
};

[[nodiscard]] VmdayoDocument loadVmdayo(const std::filesystem::path& path);
[[nodiscard]] VmdayoDocument parseVmdayo(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::vector<VmdayoDocument> parseVmdayoSubsets(
    std::span<const std::uint8_t> bytes, std::size_t count);
[[nodiscard]] std::vector<std::uint8_t> serializeVmdayo(const VmdayoDocument& document);
[[nodiscard]] std::vector<std::uint8_t> serializeVmdayoSubset(const VmdayoDocument& document);
void saveVmdayo(const std::filesystem::path& path, const VmdayoDocument& document);

} // namespace dayo::core
