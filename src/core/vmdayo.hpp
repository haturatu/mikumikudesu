#pragma once

#include "core/motion.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dayo::core {

// .vmdayo is an auxiliary v3 motion document. Unknown fields are retained in
// opaque so importing and exporting a project never destroys newer data.
struct VmdayoDocument {
    int version { 1 };
    std::string modelName;
    MotionDocument motion;
    std::vector<std::uint8_t> opaque;
};

[[nodiscard]] VmdayoDocument loadVmdayo(const std::filesystem::path& path);
void saveVmdayo(const std::filesystem::path& path, const VmdayoDocument& document);

} // namespace dayo::core
