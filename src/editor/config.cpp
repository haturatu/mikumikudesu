#include "editor/config.hpp"

#include <fstream>

namespace dayo::editor {

void EditorConfig::load(const std::filesystem::path& path) noexcept {
    try {
        std::ifstream input(path);
        if (!input)
            return;
        std::string key;
        std::string value;
        std::string line;
        while (std::getline(input, line)) {
            const auto separator = line.find('=');
            if (separator == std::string::npos)
                continue;
            key = line.substr(0, separator);
            value = line.substr(separator + 1);
            if (key == "language")
                language = value;
            else if (key == "theme")
                theme = value;
            else if (key == "autosave")
                autosave = (value == "1" || value == "true");
            else if (key == "vsync")
                vsync = (value == "1" || value == "true");
        }
    } catch (...) {
    }
}

void EditorConfig::save(const std::filesystem::path& path) const noexcept {
    try {
        std::error_code error;
        if (path.has_parent_path())
            std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream output(path, std::ios::trunc);
        if (!output)
            return;
        output << "language=" << language << "\ntheme=" << theme << "\nautosave=" << (autosave ? "1" : "0")
               << "\nvsync=" << (vsync ? "1" : "0") << '\n';
    } catch (...) {
    }
}

std::filesystem::path EditorConfig::defaultPath() {
    return std::filesystem::path("config") / "fxedit.cfg";
}

} // namespace dayo::editor
