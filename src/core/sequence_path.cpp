#include "core/sequence_path.hpp"

#include <cctype>
#include <cstdio>

namespace dayo::core {

std::optional<SequencePathSpec> parseSequencePath(const std::filesystem::path& path) noexcept {
    try {
        const std::string filename = path.filename().string();
        const auto dot = filename.rfind('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= filename.size())
            return std::nullopt;
        std::string stem = filename.substr(0, dot);
        const std::string extension = filename.substr(dot);
        std::size_t digits = 0;
        while (!stem.empty() && std::isdigit(static_cast<unsigned char>(stem.back())) != 0) {
            stem.pop_back();
            ++digits;
        }
        if (digits == 0 || stem.empty())
            return std::nullopt;
        const std::string number = filename.substr(stem.size(), digits);
        SequencePathSpec spec;
        spec.prefix = stem;
        spec.digits = static_cast<std::uint32_t>(digits);
        spec.extension = extension;
        try {
            spec.start = static_cast<std::uint32_t>(std::stoul(number));
        } catch (...) {
            return std::nullopt;
        }
        return spec;
    } catch (...) {
        return std::nullopt;
    }
}

std::filesystem::path formatSequencePath(const std::filesystem::path& directory, const SequencePathSpec& spec,
                                          std::uint32_t frame) {
    char name[256]{};
    std::snprintf(name, sizeof(name), "%s%0*u%s", spec.prefix.c_str(), static_cast<int>(spec.digits), frame,
                  spec.extension.c_str());
    if (directory.empty())
        return std::filesystem::path(name);
    return directory / name;
}

std::filesystem::path formatSequencePath(const SequencePathSpec& spec, std::uint32_t frame) {
    return formatSequencePath(std::filesystem::path{}, spec, frame);
}

} // namespace dayo::core
