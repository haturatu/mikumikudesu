#pragma once

#include "core/motion.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dayo::core {

// This is mikumikudesu's VMdayo-like auxiliary v3 motion document. It is not
// claimed to be binary-compatible with the proprietary Windows 1.30 writer.
// Unknown/upstream bytes are retained in opaque so round-tripping never
// destroys data that this reader does not understand.
struct VmdayoDocument {
    int version { 1 };
    std::string modelName;
    MotionDocument motion;
    std::vector<std::uint8_t> opaque;
};

[[nodiscard]] VmdayoDocument loadVmdayo(const std::filesystem::path& path);
void saveVmdayo(const std::filesystem::path& path, const VmdayoDocument& document);

} // namespace dayo::core
