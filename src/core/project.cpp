#include "core/project.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <cctype>
#include <stdexcept>
#include <string>
#include <system_error>

namespace dayo::core {
namespace {

using Json = nlohmann::json;

std::filesystem::path resolveAsset(const std::filesystem::path& base,
                                   const std::filesystem::path& value) {
    if (value.empty()) return {};
    if (value.is_absolute()) return value.lexically_normal();
    return std::filesystem::absolute(base / value).lexically_normal();
}

std::filesystem::path portablePath(const std::filesystem::path& base,
                                   const std::filesystem::path& value) {
    if (value.empty()) return {};
    std::error_code error;
    const auto relative = std::filesystem::relative(value, base, error);
    if (!error && !relative.empty()) return relative;
    return value;
}

Json parseHeader(std::ifstream& input, const std::filesystem::path& path) {
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    const auto begin = contents.find('{');
    const auto marker = contents.find("[BinaryDayo]");
    const auto end = marker == std::string::npos ? contents.rfind('}')
                                                  : contents.rfind('}', marker);
    if (begin == std::string::npos || end == std::string::npos || end < begin) {
        throw std::runtime_error("invalid .dayo JSON header: " + path.string());
    }
    return Json::parse(contents.substr(begin, end - begin + 1));
}

void appendIfPresent(DayoProject& result, const Json& object, std::string_view field,
                     std::string kind, const std::filesystem::path& base) {
    const std::string key(field);
    if (!object.contains(key) || !object.at(key).is_string()) return;
    auto value = object.at(key).get<std::string>();
    if (!value.empty()) result.assets.push_back(ProjectAsset { std::move(kind), resolveAsset(base, value) });
}

} // namespace

DayoProject loadProject(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open project: " + path.string());
    const auto root = parseHeader(input, path);
    const auto base = std::filesystem::absolute(path).parent_path();

    DayoProject result;
    if (root.contains("mikumikudesu")) {
        const auto& native = root.at("mikumikudesu");
        result.version = native.value("version", 2);
        if (result.version > 2) {
            throw std::runtime_error("unsupported .dayo project version "
                                     + std::to_string(result.version));
        }
        result.renderer = native.value("renderer", "preview");
        result.frame = native.value("frame", 0.0F);
        result.playing = native.value("playing", true);
        if (native.contains("assets") && native.at("assets").is_array()) {
            for (const auto& asset : native.at("assets")) {
                if (!asset.is_object() || !asset.contains("path") || !asset.at("path").is_string()) continue;
                result.assets.push_back({ asset.value("kind", "unknown"),
                                          resolveAsset(base, asset.at("path").get<std::string>()) });
            }
        }
        return result;
    }

    // Compatibility with the public Windows format. Its keyframes follow the
    // JSON as a binary stream; assets and editor state remain recoverable here.
    if (!root.contains("MikuMikuDayo") || !root.at("MikuMikuDayo").is_object()) {
        throw std::runtime_error("unrecognized .dayo project: " + path.string());
    }
    const auto& legacy = root.at("MikuMikuDayo");
    const auto legacyBase = resolveAsset(base, legacy.value("assetPath", "."));
    result.version = legacy.value("ver", 1);
    if (legacy.contains("editor") && legacy.at("editor").is_object()) {
        const auto& editor = legacy.at("editor");
        result.frame = editor.value("frame", 0.0F);
        appendIfPresent(result, editor, "wavFile", "audio", legacyBase);
        appendIfPresent(result, editor, "movieFile", "video", legacyBase);
    }
    if (legacy.contains("models") && legacy.at("models").is_array()) {
        for (const auto& model : legacy.at("models")) {
            appendIfPresent(result, model, "filename", "pmx", legacyBase);
        }
    }
    if (legacy.contains("fxinfo") && legacy.at("fxinfo").is_array()) {
        for (const auto& effect : legacy.at("fxinfo")) {
            appendIfPresent(result, effect, "filename", "effect", legacyBase);
            if (effect.contains("filename") && effect.at("filename").is_string()) {
                auto name = std::filesystem::path(effect.at("filename").get<std::string>()).stem().string();
                for (auto& character : name) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                if (name == "subayai" || name == "bdpt" || name == "preview") result.renderer = name;
            }
        }
    }
    return result;
}

void saveProject(const std::filesystem::path& path, const DayoProject& project) {
    if (path.empty()) throw std::invalid_argument("project path is empty");
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    const auto base = absolute.parent_path();
    std::filesystem::create_directories(base);

    Json assets = Json::array();
    for (const auto& asset : project.assets) {
        assets.push_back({ { "kind", asset.kind },
                           { "path", portablePath(base, asset.path).generic_string() } });
    }
    const Json root {
        { "mikumikudesu", {
            { "version", 2 },
            { "renderer", project.renderer },
            { "frame", project.frame },
            { "playing", project.playing },
            { "assets", std::move(assets) },
        } },
    };

    auto temporary = absolute;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot write project: " + temporary.string());
        output << "[MikuMikuDayo]\n" << root.dump(2) << "\n[BinaryDayo]\n";
        output.flush();
        if (!output) throw std::runtime_error("failed while writing project: " + temporary.string());
    }
    std::error_code error;
    std::filesystem::rename(temporary, absolute, error);
    if (error) {
        std::filesystem::remove(absolute, error);
        error.clear();
        std::filesystem::rename(temporary, absolute, error);
    }
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("cannot replace project: " + error.message());
    }
}

} // namespace dayo::core
