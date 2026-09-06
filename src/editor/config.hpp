#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace dayo::editor {

// Minimal persistent editor preferences. Stored as a tiny key=value file;
// failures fall back to defaults and never throw out of load().
struct EditorConfig {
    std::string language{"en"};
    std::string theme{"dark"};
    float timelineFps{30.0F};
    bool autosave{true};
    std::uint32_t autosaveMinutes{10};
    bool vsync{true};

    void load(const std::filesystem::path& path) noexcept;
    void save(const std::filesystem::path& path) const noexcept;
    static std::filesystem::path defaultPath();
};

} // namespace dayo::editor
