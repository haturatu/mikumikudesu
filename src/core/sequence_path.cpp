#include "core/sequence_path.hpp"

#include <cctype>
#include <charconv>
#include <limits>
#include <stdexcept>

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
        if (digits == 0 || digits > 1024)
            return std::nullopt;
        const std::string number = filename.substr(stem.size(), digits);
        SequencePathSpec spec;
        spec.prefix = stem;
        spec.digits = static_cast<std::uint32_t>(digits);
        spec.extension = extension;
        const auto converted = std::from_chars(number.data(), number.data() + number.size(), spec.start);
        if (converted.ec != std::errc{} || converted.ptr != number.data() + number.size())
            return std::nullopt;
        return spec;
    } catch (...) {
        return std::nullopt;
    }
}

std::filesystem::path formatSequencePath(const std::filesystem::path& directory, const SequencePathSpec& spec,
                                         std::uint32_t frame) {
    if (spec.digits > 1024)
        throw std::invalid_argument("sequence digit width exceeds 1024");
    auto number = std::to_string(frame);
    if (number.size() < spec.digits)
        number.insert(0, spec.digits - number.size(), '0');
    const auto name = spec.prefix + number + spec.extension;
    if (directory.empty())
        return std::filesystem::path(name);
    return directory / name;
}

std::filesystem::path formatSequencePath(const SequencePathSpec& spec, std::uint32_t frame) {
    return formatSequencePath(std::filesystem::path{}, spec, frame);
}

} // namespace dayo::core
