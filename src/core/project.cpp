#include "core/project.hpp"
#include "core/vmdayo.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <cctype>
#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>
#include <cstring>

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

std::vector<std::uint8_t> readBinarySection(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto marker = contents.find("[BinaryDayo]");
    if (marker == std::string::npos) return {};
    auto begin = marker + std::strlen("[BinaryDayo]");
    while (begin < contents.size() && (contents[begin] == '\r' || contents[begin] == '\n')) ++begin;
    return std::vector<std::uint8_t>(contents.begin() + static_cast<std::ptrdiff_t>(begin), contents.end());
}

void appendIfPresent(DayoProject& result, const Json& object, std::string_view field,
                     std::string kind, const std::filesystem::path& base) {
    const std::string key(field);
    if (!object.contains(key) || !object.at(key).is_string()) return;
    auto value = object.at(key).get<std::string>();
    if (!value.empty()) result.assets.push_back(ProjectAsset { std::move(kind), resolveAsset(base, value) });
}

class BinaryReader {
public:
    BinaryReader(std::string_view bytes, int version) : bytes_(bytes), version_(version) {}

    template <typename T>
    T value(std::string_view field) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (cursor_ + sizeof(T) > bytes_.size()) fail(field);
        T result {};
        std::memcpy(&result, bytes_.data() + cursor_, sizeof(T));
        cursor_ += sizeof(T);
        return result;
    }

    std::string text(std::string_view field) {
        const auto count = value<std::int32_t>(field);
        if (count < 0 || count > 1'000'000) fail(field);
        if (version_ != 0) {
            const auto size = static_cast<std::size_t>(count);
            if (cursor_ + size > bytes_.size()) fail(field);
            std::string result(bytes_.substr(cursor_, size));
            cursor_ += size;
            return result;
        }
        const auto size = static_cast<std::size_t>(count) * 2U;
        if (cursor_ + size > bytes_.size()) fail(field);
        std::string result;
        result.reserve(static_cast<std::size_t>(count) * 3U);
        for (std::int32_t i = 0; i < count; ++i) {
            const auto lo = static_cast<std::uint8_t>(bytes_[cursor_++]);
            const auto hi = static_cast<std::uint8_t>(bytes_[cursor_++]);
            std::uint32_t codepoint = static_cast<std::uint16_t>(lo | (hi << 8U));
            if (codepoint >= 0xD800U && codepoint <= 0xDBFFU && i + 1 < count) {
                const auto lo2 = static_cast<std::uint8_t>(bytes_[cursor_++]);
                const auto hi2 = static_cast<std::uint8_t>(bytes_[cursor_++]);
                ++i;
                const auto trail = static_cast<std::uint16_t>(lo2 | (hi2 << 8U));
                if (trail >= 0xDC00U && trail <= 0xDFFFU) {
                    codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) + trail - 0xDC00U;
                }
            }
            if (codepoint <= 0x7FU) result.push_back(static_cast<char>(codepoint));
            else if (codepoint <= 0x7FFU) {
                result.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
                result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
            } else if (codepoint <= 0xFFFFU) {
                result.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
                result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
                result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
            } else {
                result.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
                result.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
                result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
                result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
            }
        }
        return result;
    }

    std::int32_t count(std::string_view field) {
        const auto result = value<std::int32_t>(field);
        if (result < 0 || result > 10'000'000) fail(field);
        return result;
    }

private:
    [[noreturn]] static void fail(std::string_view field) {
        throw std::runtime_error("invalid legacy .dayo binary " + std::string(field));
    }
    std::string_view bytes_;
    std::size_t cursor_ {};
    int version_ {};
};

VmdMotion readSubset(BinaryReader& reader) {
    VmdMotion motion;
    for (std::int32_t i = 0, count = reader.count("bone count"); i < count; ++i) {
        VmdBoneKey key;
        key.name = reader.text("bone name");
        key.frame = static_cast<std::uint32_t>(reader.value<std::int32_t>("bone frame"));
        key.translation = reader.value<Float3>("bone translation");
        key.rotation = reader.value<Float4>("bone rotation");
        const auto interpolation = reader.value<std::array<std::uint8_t, 16>>("bone interpolation");
        // The first 16 bytes use the same de-interleaved controls as VMD.
        for (std::size_t channel = 0; channel < 4; ++channel) {
            key.interpolation[channel] = interpolation[channel * 4];
            key.interpolation[channel + 4] = interpolation[channel * 4 + 1];
            key.interpolation[channel + 8] = interpolation[channel * 4 + 2];
            key.interpolation[channel + 12] = interpolation[channel * 4 + 3];
        }
        static_cast<void>(reader.value<std::uint8_t>("bone physics"));
        motion.lastFrame = std::max(motion.lastFrame, key.frame);
        motion.bones.push_back(std::move(key));
    }
    for (std::int32_t i = 0, count = reader.count("morph count"); i < count; ++i) {
        VmdMorphKey key;
        key.name = reader.text("morph name");
        key.frame = static_cast<std::uint32_t>(reader.value<std::int32_t>("morph frame"));
        key.weight = reader.value<float>("morph value");
        motion.lastFrame = std::max(motion.lastFrame, key.frame);
        motion.morphs.push_back(std::move(key));
    }
    for (std::int32_t i = 0, count = reader.count("camera count"); i < count; ++i) {
        VmdCameraKey key;
        key.frame = static_cast<std::uint32_t>(reader.value<std::int32_t>("camera frame"));
        key.distance = reader.value<float>("camera distance");
        key.position = reader.value<Float3>("camera target");
        key.rotation = reader.value<Float3>("camera rotation");
        const auto internal = reader.value<std::array<std::uint8_t, 24>>("camera interpolation");
        for (std::size_t channel = 0; channel < 6; ++channel) {
            key.interpolation[channel * 4] = internal[channel * 4];
            key.interpolation[channel * 4 + 1] = internal[channel * 4 + 2];
            key.interpolation[channel * 4 + 2] = internal[channel * 4 + 1];
            key.interpolation[channel * 4 + 3] = internal[channel * 4 + 3];
        }
        key.viewAngle = static_cast<std::uint32_t>(std::max(reader.value<float>("camera view angle"), 0.0F));
        key.perspective = reader.value<std::uint8_t>("camera perspective") != 0;
        static_cast<void>(reader.value<std::int32_t>("camera parent model"));
        static_cast<void>(reader.value<std::int32_t>("camera parent bone"));
        motion.lastFrame = std::max(motion.lastFrame, key.frame);
        motion.cameras.push_back(key);
    }
    for (std::int32_t i = 0, count = reader.count("light count"); i < count; ++i) {
        VmdLightKey key;
        key.frame = static_cast<std::uint32_t>(reader.value<std::int32_t>("light frame"));
        key.color = reader.value<Float3>("light color");
        key.position = reader.value<Float3>("light direction");
        motion.lastFrame = std::max(motion.lastFrame, key.frame);
        motion.lights.push_back(key);
    }
    for (std::int32_t i = 0, count = reader.count("shadow count"); i < count; ++i) {
        VmdShadowKey key;
        key.frame = static_cast<std::uint32_t>(reader.value<std::int32_t>("shadow frame"));
        key.mode = reader.value<std::uint8_t>("shadow mode");
        key.distance = reader.value<float>("shadow distance");
        motion.lastFrame = std::max(motion.lastFrame, key.frame);
        motion.shadows.push_back(key);
    }
    for (std::int32_t i = 0, count = reader.count("extra count"); i < count; ++i) {
        const auto frame = static_cast<std::uint32_t>(reader.value<std::int32_t>("extra frame"));
        VmdIkKey key { .frame = frame,
                       .visible = reader.value<std::uint8_t>("visibility") != 0,
                       .states = {} };
        for (std::int32_t j = 0, states = reader.count("IK count"); j < states; ++j) {
            key.states.push_back({ reader.text("IK name"), reader.value<std::uint8_t>("IK enabled") != 0 });
        }
        for (std::int32_t j = 0, parents = reader.count("external parent count"); j < parents; ++j) {
            static_cast<void>(reader.value<std::int32_t>("external parent model"));
            static_cast<void>(reader.text("external parent bone"));
            static_cast<void>(reader.text("external child bone"));
        }
        motion.lastFrame = std::max(motion.lastFrame, frame);
        motion.ik.push_back(std::move(key));
    }
    for (std::int32_t i = 0, count = reader.count("gravity count"); i < count; ++i) {
        const auto frame = static_cast<std::uint32_t>(reader.value<std::int32_t>("gravity frame"));
        static_cast<void>(reader.value<float>("gravity strength"));
        static_cast<void>(reader.value<Float3>("gravity direction"));
        static_cast<void>(reader.value<float>("gravity noise amplitude"));
        static_cast<void>(reader.value<float>("gravity noise frequency"));
        motion.lastFrame = std::max(motion.lastFrame, frame);
    }
    return motion;
}

std::vector<VmdMotion> readLegacyMotions(const std::filesystem::path& path, int version,
                                         std::size_t modelCount) {
    std::ifstream input(path, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto marker = contents.find("[BinaryDayo]");
    if (marker == std::string::npos) return {};
    auto begin = marker + std::strlen("[BinaryDayo]");
    while (begin < contents.size() && (contents[begin] == '\r' || contents[begin] == '\n')) ++begin;
    BinaryReader reader(std::string_view(contents).substr(begin), version);
    std::vector<VmdMotion> result;
    result.reserve(modelCount + 1);
    result.push_back(readSubset(reader)); // camera/light subset
    for (std::size_t model = 0; model < modelCount; ++model) result.push_back(readSubset(reader));
    return result;
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
        if (result.version > 3) {
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
        result.embeddedVmdayo = readBinarySection(path);
        if (!result.embeddedVmdayo.empty()) {
            const auto vmdayo = parseVmdayo(result.embeddedVmdayo);
            if (vmdayo.opaque.empty()) result.embeddedMotion = toVmdMotion(vmdayo.motion, vmdayo.modelName);
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
    const auto modelCount = legacy.contains("models") && legacy.at("models").is_array()
        ? legacy.at("models").size() : std::size_t {};
    const auto motions = readLegacyMotions(path, result.version, modelCount);
    if (!motions.empty()) {
        result.embeddedMotion = motions.size() > 1 ? motions[1] : motions[0];
        auto& embedded = *result.embeddedMotion;
        embedded.cameras = motions[0].cameras;
        embedded.lights = motions[0].lights;
        embedded.shadows = motions[0].shadows;
        embedded.lastFrame = std::max(embedded.lastFrame, motions[0].lastFrame);
    }
    result.embeddedVmdayo = readBinarySection(path);
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
            { "version", 3 },
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
        auto payload = project.embeddedVmdayo;
        if (payload.empty() && project.embeddedMotion) {
            payload = serializeVmdayo({ 3, project.embeddedMotion->modelName,
                toMotionDocument(*project.embeddedMotion), {} });
        }
        if (!payload.empty()) {
            output.write(reinterpret_cast<const char*>(payload.data()),
                         static_cast<std::streamsize>(payload.size()));
        }
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
