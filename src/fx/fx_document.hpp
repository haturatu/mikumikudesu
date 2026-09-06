#pragma once

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace dayo::fx {

// Raw effect source preserved byte-for-byte. "生保持": no interpretation,
// no Jsonnet expansion, no HLSL preprocessing. Parsing/linking happens in
// FxCompiler so watchers and caches can key on the raw payload.
struct FxSourceDocument {
    std::filesystem::path path;
    std::string raw;
    std::uint64_t version{};

    [[nodiscard]] bool empty() const noexcept {
        return raw.empty();
    }
};

[[nodiscard]] inline FxSourceDocument loadFxSourceDocument(const std::filesystem::path& path,
                                                           std::uint64_t version = 0) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open fx source: " + path.string());
    std::string raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    FxSourceDocument document;
    document.path = path;
    document.raw = std::move(raw);
    document.version = version;
    return document;
}

[[nodiscard]] inline FxSourceDocument makeFxSourceDocument(std::filesystem::path path, std::string raw,
                                                           std::uint64_t version = 0) {
    FxSourceDocument document;
    document.path = std::move(path);
    document.raw = std::move(raw);
    document.version = version;
    return document;
}

} // namespace dayo::fx
