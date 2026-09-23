#include "core/project.hpp"
#include "core/vmdayo.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <system_error>

namespace dayo::core {
namespace {

using Json = nlohmann::json;

std::filesystem::path resolveAsset(const std::filesystem::path& base, const std::filesystem::path& value) {
    if (value.empty())
        return {};
    if (value.is_absolute())
        return value.lexically_normal();
    return std::filesystem::absolute(base / value).lexically_normal();
}

std::filesystem::path portablePath(const std::filesystem::path& base, const std::filesystem::path& value) {
    if (value.empty())
        return {};
    std::error_code error;
    const auto relative = std::filesystem::relative(value, base, error);
    if (!error && !relative.empty())
        return relative;
    return value;
}

Json parseHeader(std::ifstream& input, const std::filesystem::path& path) {
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto begin = contents.find('{');
    const auto marker = contents.find("[BinaryDayo]");
    const auto end = marker == std::string::npos ? contents.rfind('}') : contents.rfind('}', marker);
    if (begin == std::string::npos || end == std::string::npos || end < begin) {
        throw std::runtime_error("invalid .dayo JSON header: " + path.string());
    }
    return Json::parse(contents.substr(begin, end - begin + 1));
}

std::vector<std::uint8_t> readBinarySection(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto marker = contents.find("[BinaryDayo]");
    if (marker == std::string::npos)
        return {};
    auto begin = marker + std::strlen("[BinaryDayo]");
    while (begin < contents.size() && (contents[begin] == '\r' || contents[begin] == '\n'))
        ++begin;
    return std::vector<std::uint8_t>(contents.begin() + static_cast<std::ptrdiff_t>(begin), contents.end());
}

void appendIfPresent(DayoProject& result, const Json& object, std::string_view field, std::string kind,
                     const std::filesystem::path& base) {
    const std::string key(field);
    if (!object.contains(key) || !object.at(key).is_string())
        return;
    auto value = object.at(key).get<std::string>();
    if (!value.empty())
        result.assets.push_back(ProjectAsset{std::move(kind), resolveAsset(base, value)});
}

std::vector<std::string> readStrings(const Json& value) {
    std::vector<std::string> result;
    if (!value.is_array())
        return result;
    for (const auto& item : value)
        if (item.is_string())
            result.push_back(item.get<std::string>());
    return result;
}

void readModelOrder(const Json& editor, std::string_view field, std::vector<ProjectModelState>& models,
                    std::int32_t ProjectModelState::* member) {
    const auto key = std::string(field);
    if (!editor.contains(key) || !editor.at(key).is_array())
        return;
    std::int32_t priority = 0;
    for (const auto& item : editor.at(key)) {
        if (!item.is_number_integer())
            continue;
        const auto index = item.get<std::int64_t>();
        if (index >= 0 && static_cast<std::uint64_t>(index) < models.size())
            models[static_cast<std::size_t>(index)].*member = priority++;
    }
}

Json modelOrderArray(const std::vector<ProjectModelState>& models, std::int32_t ProjectModelState::* member) {
    std::vector<std::size_t> order(models.size());
    std::iota(order.begin(), order.end(), 0U);
    std::ranges::stable_sort(
        order, [&](const auto left, const auto right) { return models[left].*member < models[right].*member; });
    Json result = Json::array();
    for (const auto index : order)
        result.push_back(index);
    return result;
}

ProjectModelState readNativeModelState(const Json& item, const std::filesystem::path& base) {
    ProjectModelState state;
    if (item.contains("source") && item.at("source").is_string())
        state.source = resolveAsset(base, item.at("source").get<std::string>());
    if (item.contains("upstreamId") && item.at("upstreamId").is_number_integer())
        state.upstreamId = item.at("upstreamId").get<std::int32_t>();
    state.bones = readStrings(item.value("bones", Json::array()));
    state.morphs = readStrings(item.value("morphs", Json::array()));
    state.materials = readStrings(item.value("materials", Json::array()));
    state.materialAnnotations = readStrings(item.value("materialAnnotations", Json::array()));
    state.motionOrder = item.value("motionOrder", state.motionOrder);
    state.deformOrder = item.value("deformOrder", state.deformOrder);
    state.postprocessOrder = item.value("postprocessOrder", state.postprocessOrder);
    state.rasterOrder = item.value("rasterOrder", state.rasterOrder);
    state.cloneCount = std::max(item.value("cloneCount", state.cloneCount), 1U);
    state.visible = item.value("visible", state.visible);
    return state;
}

DayoProject readUpstreamJson(const Json& root, const std::filesystem::path& base) {
    DayoProject result;
    if (!root.contains("MikuMikuDayo") || !root.at("MikuMikuDayo").is_object())
        return result;

    const auto& legacy = root.at("MikuMikuDayo");
    const auto legacyBase = resolveAsset(base, legacy.value("assetPath", "."));
    result.version = legacy.value("ver", 1);
    if (legacy.contains("editor") && legacy.at("editor").is_object()) {
        const auto& editor = legacy.at("editor");
        result.frame = editor.value("frame", 0.0F);
        result.editor.samplesPerFrame = std::max(editor.value("samplesPerFrame", result.editor.samplesPerFrame), 1U);
        result.editor.motionBlur = editor.value("motionBlur", result.editor.motionBlur);
        result.editor.outputWidth = std::max(editor.value("outputWidth", result.editor.outputWidth), 1U);
        result.editor.outputHeight = std::max(editor.value("outputHeight", result.editor.outputHeight), 1U);
        result.editor.recordFps = editor.value("recordFps", result.editor.recordFps);
        result.editor.animationSpeed = editor.value("animationSpeed", result.editor.animationSpeed);
        appendIfPresent(result, editor, "wavFile", "audio", legacyBase);
        appendIfPresent(result, editor, "movieFile", "video", legacyBase);
    }

    std::vector<std::int32_t> modelIds;
    if (legacy.contains("models") && legacy.at("models").is_array()) {
        for (const auto& model : legacy.at("models")) {
            if (!model.is_object()) {
                const auto fallbackId = static_cast<std::int32_t>(modelIds.size() + 1U);
                modelIds.push_back(fallbackId);
                ProjectModelState state;
                state.motionOrder = state.deformOrder = state.postprocessOrder = state.rasterOrder =
                    static_cast<std::int32_t>(result.models.size());
                result.models.push_back(std::move(state));
                continue;
            }
            const auto modelPath = model.contains("filename") && model.at("filename").is_string()
                                       ? resolveAsset(legacyBase, model.at("filename").get<std::string>())
                                       : std::filesystem::path{};
            if (!modelPath.empty())
                result.assets.emplace_back("pmx", modelPath);
            ProjectModelState state;
            state.source = modelPath;
            state.upstreamId = model.value("id", static_cast<std::int32_t>(modelIds.size() + 1U));
            state.bones = readStrings(model.value("bones", Json::array()));
            state.morphs = readStrings(model.value("morphs", Json::array()));
            state.materials = readStrings(model.value("materials", Json::array()));
            state.motionOrder = state.deformOrder = state.postprocessOrder = state.rasterOrder =
                static_cast<std::int32_t>(result.models.size());
            modelIds.push_back(*state.upstreamId);
            result.models.push_back(std::move(state));
        }
        if (legacy.contains("editor") && legacy.at("editor").is_object()) {
            const auto& editor = legacy.at("editor");
            readModelOrder(editor, "motionOrder", result.models, &ProjectModelState::motionOrder);
            readModelOrder(editor, "deformOrder", result.models, &ProjectModelState::deformOrder);
            readModelOrder(editor, "postprocessOrder", result.models, &ProjectModelState::postprocessOrder);
            readModelOrder(editor, "rasterOrder", result.models, &ProjectModelState::rasterOrder);
        }
    }

    if (legacy.contains("fxinfo") && legacy.at("fxinfo").is_array()) {
        for (const auto& effect : legacy.at("fxinfo")) {
            if (!effect.is_object() || !effect.contains("filename") || !effect.at("filename").is_string())
                continue;
            ProjectAsset loaded{"effect", resolveAsset(legacyBase, effect.at("filename").get<std::string>())};
            loaded.upstreamId = effect.value("id", -1);
            if (*loaded.upstreamId >= 0) {
                const auto owner = std::ranges::find(modelIds, *loaded.upstreamId);
                if (owner != modelIds.end())
                    loaded.ownerModelIndex = static_cast<std::size_t>(std::distance(modelIds.begin(), owner));
            }
            if (effect.contains("mdb") && effect.at("mdb").is_object() && effect.at("mdb").contains("sourceFiles") &&
                effect.at("mdb").at("sourceFiles").is_array()) {
                for (const auto& modelSources : effect.at("mdb").at("sourceFiles"))
                    loaded.materialSourceFiles.push_back(readStrings(modelSources));
            }
            auto name = loaded.path.stem().string();
            std::ranges::transform(name, name.begin(),
                                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            if (loaded.upstreamId == -1 && (name == "subayai" || name == "bdpt" || name == "preview"))
                result.renderer = name;
            result.assets.push_back(std::move(loaded));
        }
    }
    return result;
}

class BinaryReader {
  public:
    BinaryReader(std::string_view bytes, int version) : bytes_(bytes), version_(version) {}

    template <typename T> T value(std::string_view field) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (cursor_ + sizeof(T) > bytes_.size())
            fail(field);
        T result{};
        std::memcpy(&result, bytes_.data() + cursor_, sizeof(T));
        cursor_ += sizeof(T);
        return result;
    }

    std::string text(std::string_view field) {
        const auto count = value<std::int32_t>(field);
        if (count < 0 || count > 1'000'000)
            fail(field);
        if (version_ != 0) {
            const auto size = static_cast<std::size_t>(count);
            if (cursor_ + size > bytes_.size())
                fail(field);
            std::string result(bytes_.substr(cursor_, size));
            cursor_ += size;
            return result;
        }
        const auto size = static_cast<std::size_t>(count) * 2U;
        if (cursor_ + size > bytes_.size())
            fail(field);
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
            if (codepoint <= 0x7FU)
                result.push_back(static_cast<char>(codepoint));
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
        if (result < 0 || result > 10'000'000)
            fail(field);
        return result;
    }

  private:
    [[noreturn]] static void fail(std::string_view field) {
        throw std::runtime_error("invalid legacy .dayo binary " + std::string(field));
    }
    std::string_view bytes_;
    std::size_t cursor_{};
    int version_{};
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
        key.physics = reader.value<std::uint8_t>("bone physics") != 0;
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
        key.viewAngle = std::max(reader.value<float>("camera view angle"), 0.0F);
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
        VmdIkKey key{.frame = frame, .visible = reader.value<std::uint8_t>("visibility") != 0, .states = {}};
        for (std::int32_t j = 0, states = reader.count("IK count"); j < states; ++j) {
            key.states.push_back({reader.text("IK name"), reader.value<std::uint8_t>("IK enabled") != 0});
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

std::vector<VmdMotion> readLegacyMotions(const std::filesystem::path& path, int version, std::size_t modelCount) {
    std::ifstream input(path, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto marker = contents.find("[BinaryDayo]");
    if (marker == std::string::npos)
        return {};
    auto begin = marker + std::strlen("[BinaryDayo]");
    while (begin < contents.size() && (contents[begin] == '\r' || contents[begin] == '\n'))
        ++begin;
    if (version >= 3) {
        const auto bytes = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(contents.data() + begin),
                                                         contents.size() - begin);
        std::vector<VmdMotion> result;
        for (auto& document : parseVmdayoSubsets(bytes, modelCount + 1U)) {
            result.push_back(toVmdMotion(std::move(document.motion), std::move(document.modelName)));
        }
        return result;
    }
    BinaryReader reader(std::string_view(contents).substr(begin), version);
    std::vector<VmdMotion> result;
    result.reserve(modelCount + 1);
    result.push_back(readSubset(reader)); // camera/light subset
    for (std::size_t model = 0; model < modelCount; ++model)
        result.push_back(readSubset(reader));
    return result;
}

} // namespace

DayoProject loadProject(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open project: " + path.string());
    const auto root = parseHeader(input, path);
    const auto base = std::filesystem::absolute(path).parent_path();
    auto upstream = readUpstreamJson(root, base);

    DayoProject result;
    if (root.contains("mikumikudesu")) {
        result.upstreamDocumentJson = root.dump();
        const auto& native = root.at("mikumikudesu");
        result.version = native.value("version", 2);
        if (result.version > 3) {
            throw std::runtime_error("unsupported .dayo project version " + std::to_string(result.version));
        }
        result.renderer = native.value("renderer", upstream.renderer);
        result.frame = native.value("frame", upstream.frame);
        result.playing = native.value("playing", true);
        if (native.contains("assets") && native.at("assets").is_array()) {
            for (const auto& asset : native.at("assets")) {
                if (!asset.is_object() || !asset.contains("path") || !asset.at("path").is_string())
                    continue;
                ProjectAsset loaded{asset.value("kind", "unknown"),
                                    resolveAsset(base, asset.at("path").get<std::string>())};
                if (asset.contains("ownerModelIndex") && asset.at("ownerModelIndex").is_number_unsigned())
                    loaded.ownerModelIndex = asset.at("ownerModelIndex").get<std::size_t>();
                if (asset.contains("upstreamId") && asset.at("upstreamId").is_number_integer())
                    loaded.upstreamId = asset.at("upstreamId").get<std::int32_t>();
                if (asset.contains("materialSourceFiles") && asset.at("materialSourceFiles").is_array()) {
                    for (const auto& modelSources : asset.at("materialSourceFiles"))
                        loaded.materialSourceFiles.push_back(readStrings(modelSources));
                }
                result.assets.push_back(std::move(loaded));
            }
        }
        if (native.contains("modelState") && native.at("modelState").is_array()) {
            for (const auto& model : native.at("modelState"))
                if (model.is_object())
                    result.models.push_back(readNativeModelState(model, base));
        }
        if (native.contains("editorState") && native.at("editorState").is_object()) {
            const auto& editor = native.at("editorState");
            result.editor.samplesPerFrame =
                std::max(editor.value("samplesPerFrame", upstream.editor.samplesPerFrame), 1U);
            result.editor.motionBlur = editor.value("motionBlur", upstream.editor.motionBlur);
            result.editor.outputWidth = std::max(editor.value("outputWidth", upstream.editor.outputWidth), 1U);
            result.editor.outputHeight = std::max(editor.value("outputHeight", upstream.editor.outputHeight), 1U);
            result.editor.recordFps = editor.value("recordFps", upstream.editor.recordFps);
            result.editor.animationSpeed = editor.value("animationSpeed", upstream.editor.animationSpeed);
        } else {
            result.editor = upstream.editor;
        }

        const auto sameAsset = [](const ProjectAsset& left, const ProjectAsset& right) {
            return left.kind == right.kind && std::filesystem::absolute(left.path).lexically_normal() ==
                                                  std::filesystem::absolute(right.path).lexically_normal();
        };
        for (auto& upstreamAsset : upstream.assets) {
            const auto found =
                std::ranges::find_if(result.assets, [&](const auto& asset) { return sameAsset(asset, upstreamAsset); });
            if (found == result.assets.end()) {
                result.assets.push_back(std::move(upstreamAsset));
                continue;
            }
            if (!found->ownerModelIndex.has_value())
                found->ownerModelIndex = upstreamAsset.ownerModelIndex;
            if (!found->upstreamId.has_value())
                found->upstreamId = upstreamAsset.upstreamId;
            if (found->materialSourceFiles.empty())
                found->materialSourceFiles = std::move(upstreamAsset.materialSourceFiles);
        }

        for (std::size_t index = 0; index < upstream.models.size(); ++index) {
            const auto& sourceState = upstream.models[index];
            auto target = result.models.end();
            if (!sourceState.source.empty()) {
                target = std::ranges::find_if(result.models, [&](const auto& state) {
                    return !state.source.empty() &&
                           std::filesystem::absolute(state.source).lexically_normal() ==
                               std::filesystem::absolute(sourceState.source).lexically_normal();
                });
            }
            if (target == result.models.end() && index < result.models.size())
                target = result.models.begin() + static_cast<std::ptrdiff_t>(index);
            if (target == result.models.end()) {
                result.models.push_back(sourceState);
                continue;
            }
            if (target->source.empty())
                target->source = sourceState.source;
            if (!target->upstreamId.has_value())
                target->upstreamId = sourceState.upstreamId;
            if (target->bones.empty())
                target->bones = sourceState.bones;
            if (target->morphs.empty())
                target->morphs = sourceState.morphs;
            if (target->materials.empty())
                target->materials = sourceState.materials;
        }

        result.embeddedVmdayo = readBinarySection(path);
        if (!result.embeddedVmdayo.empty()) {
            const auto modelCount = static_cast<std::size_t>(
                std::ranges::count_if(result.assets, [](const auto& asset) { return asset.kind == "pmx"; }));
            try {
                if (modelCount != 0U) {
                    for (auto& document : parseVmdayoSubsets(result.embeddedVmdayo, modelCount + 1U)) {
                        result.embeddedMotions.push_back(
                            toVmdMotion(std::move(document.motion), std::move(document.modelName)));
                    }
                    result.embeddedMotion =
                        result.embeddedMotions.size() > 1U ? result.embeddedMotions[1] : result.embeddedMotions[0];
                    if (result.embeddedMotions.size() > 1U) {
                        result.embeddedMotion->cameras = result.embeddedMotions[0].cameras;
                        result.embeddedMotion->lights = result.embeddedMotions[0].lights;
                        result.embeddedMotion->shadows = result.embeddedMotions[0].shadows;
                    }
                } else {
                    const auto vmdayo = parseVmdayo(result.embeddedVmdayo);
                    if (vmdayo.opaque.empty()) {
                        result.embeddedMotion = toVmdMotion(vmdayo.motion, vmdayo.modelName);
                        result.embeddedMotions.push_back(*result.embeddedMotion);
                    }
                }
            } catch (const std::exception&) {
                // Keep an unknown payload byte-for-byte; the asset and editor
                // portion of the project remains usable.
            }
        }
        return result;
    }

    // Compatibility with the public Windows format. Its keyframes follow the
    // JSON as a binary stream; assets and editor state remain recoverable here.
    if (!root.contains("MikuMikuDayo") || !root.at("MikuMikuDayo").is_object())
        throw std::runtime_error("unrecognized .dayo project: " + path.string());
    result = std::move(upstream);
    result.upstreamDocumentJson = root.dump();
    const auto modelCount =
        root.at("MikuMikuDayo").contains("models") && root.at("MikuMikuDayo").at("models").is_array()
            ? root.at("MikuMikuDayo").at("models").size()
            : std::size_t{};
    const auto motions = readLegacyMotions(path, result.version, modelCount);
    if (!motions.empty()) {
        result.embeddedMotions = motions;
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
    if (path.empty())
        throw std::invalid_argument("project path is empty");
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    const auto base = absolute.parent_path();
    std::filesystem::create_directories(base);

    Json root = Json::object();
    if (!project.upstreamDocumentJson.empty()) {
        try {
            root = Json::parse(project.upstreamDocumentJson);
        } catch (const Json::exception& exception) {
            throw std::invalid_argument("invalid preserved .dayo document: " + std::string(exception.what()));
        }
        if (!root.is_object())
            throw std::invalid_argument("preserved .dayo document root must be an object");
    }

    Json assets = Json::array();
    for (const auto& asset : project.assets) {
        Json value{{"kind", asset.kind}, {"path", portablePath(base, asset.path).generic_string()}};
        if (asset.ownerModelIndex.has_value())
            value["ownerModelIndex"] = *asset.ownerModelIndex;
        if (asset.upstreamId.has_value())
            value["upstreamId"] = *asset.upstreamId;
        if (!asset.materialSourceFiles.empty())
            value["materialSourceFiles"] = asset.materialSourceFiles;
        assets.push_back(std::move(value));
    }

    std::vector<const ProjectAsset*> modelAssets;
    for (const auto& asset : project.assets)
        if (asset.kind == "pmx")
            modelAssets.push_back(&asset);
    std::vector<ProjectModelState> modelStates;
    modelStates.reserve(modelAssets.size());
    for (std::size_t index = 0; index < modelAssets.size(); ++index) {
        const auto assetPath = std::filesystem::absolute(modelAssets[index]->path).lexically_normal();
        const auto bySource = std::ranges::find_if(project.models, [&](const auto& state) {
            return !state.source.empty() && std::filesystem::absolute(state.source).lexically_normal() == assetPath;
        });
        auto state = bySource != project.models.end()
                         ? *bySource
                         : (index < project.models.size() ? project.models[index] : ProjectModelState{});
        state.source = assetPath;
        if (!state.upstreamId.has_value() && modelAssets[index]->upstreamId.has_value())
            state.upstreamId = modelAssets[index]->upstreamId;
        modelStates.push_back(std::move(state));
    }
    const auto& oldUpstream =
        root.contains("MikuMikuDayo") && root.at("MikuMikuDayo").is_object() ? root.at("MikuMikuDayo") : Json::object();
    const auto oldModels = oldUpstream.value("models", Json::array());
    Json upstreamModels = Json::array();
    for (std::size_t index = 0; index < modelAssets.size(); ++index) {
        Json model = oldModels.is_array() && index < oldModels.size() && oldModels[index].is_object() ? oldModels[index]
                                                                                                      : Json::object();
        const auto& state = modelStates[index];
        model["cereal_class_version"] = 3;
        model["id"] = state.upstreamId.value_or(static_cast<std::int32_t>(index + 1U));
        model["filename"] = portablePath(base, modelAssets[index]->path).generic_string();
        model["bones"] = state.bones;
        model["morphs"] = state.morphs;
        model["materials"] = state.materials;
        upstreamModels.push_back(std::move(model));
    }

    const auto oldEffects = oldUpstream.value("fxinfo", Json::array());
    Json upstreamEffects = Json::array();
    for (const auto& asset : project.assets) {
        if (asset.kind != "effect")
            continue;
        const auto modelId = asset.ownerModelIndex.has_value() && *asset.ownerModelIndex < modelStates.size()
                                 ? modelStates[*asset.ownerModelIndex].upstreamId.value_or(
                                       static_cast<std::int32_t>(*asset.ownerModelIndex + 1U))
                                 : -1;
        const auto id = asset.upstreamId.value_or(modelId);
        Json effect = Json::object();
        if (oldEffects.is_array()) {
            const auto previous = std::ranges::find_if(oldEffects, [&](const auto& value) {
                return value.is_object() && value.value("id", std::numeric_limits<std::int32_t>::min()) == id;
            });
            if (previous != oldEffects.end())
                effect = *previous;
        }
        effect["id"] = id;
        effect["filename"] = portablePath(base, asset.path).generic_string();
        Json backup = effect.value("mdb", Json::object());
        if (!backup.is_object())
            backup = Json::object();
        backup["sourceFiles"] = asset.materialSourceFiles;
        effect["mdb"] = std::move(backup);
        upstreamEffects.push_back(std::move(effect));
    }

    Json upstream = oldUpstream;
    upstream["cereal_class_version"] = 3;
    upstream["ver"] = 3;
    upstream["assetPath"] = base.generic_string();
    upstream["dayoVer"] = 130;
    upstream["models"] = std::move(upstreamModels);
    upstream["fxinfo"] = std::move(upstreamEffects);
    Json upstreamEditor = upstream.value("editor", Json::object());
    if (!upstreamEditor.is_object())
        upstreamEditor = Json::object();
    upstreamEditor["cereal_class_version"] = 3;
    upstreamEditor["frame"] = static_cast<int>(project.frame);
    upstreamEditor["samplesPerFrame"] = std::max(project.editor.samplesPerFrame, 1U);
    upstreamEditor["motionBlur"] = project.editor.motionBlur;
    upstreamEditor["outputWidth"] = std::max(project.editor.outputWidth, 1U);
    upstreamEditor["outputHeight"] = std::max(project.editor.outputHeight, 1U);
    upstreamEditor["recordFps"] = project.editor.recordFps;
    upstreamEditor["animationSpeed"] = project.editor.animationSpeed;
    upstreamEditor["motionOrder"] = modelOrderArray(modelStates, &ProjectModelState::motionOrder);
    upstreamEditor["postprocessOrder"] = modelOrderArray(modelStates, &ProjectModelState::postprocessOrder);
    upstreamEditor["deformOrder"] = modelOrderArray(modelStates, &ProjectModelState::deformOrder);
    upstreamEditor["rasterOrder"] = modelOrderArray(modelStates, &ProjectModelState::rasterOrder);
    upstream["editor"] = std::move(upstreamEditor);
    root["MikuMikuDayo"] = std::move(upstream);

    Json native = root.value("mikumikudesu", Json::object());
    if (!native.is_object())
        native = Json::object();
    native["version"] = 3;
    native["renderer"] = project.renderer;
    native["frame"] = project.frame;
    native["playing"] = project.playing;
    native["assets"] = std::move(assets);
    native["editorState"] = {{"samplesPerFrame", std::max(project.editor.samplesPerFrame, 1U)},
                             {"motionBlur", project.editor.motionBlur},
                             {"outputWidth", std::max(project.editor.outputWidth, 1U)},
                             {"outputHeight", std::max(project.editor.outputHeight, 1U)},
                             {"recordFps", project.editor.recordFps},
                             {"animationSpeed", project.editor.animationSpeed}};
    native["modelState"] = Json::array();
    for (const auto& model : modelStates) {
        Json state{{"source", portablePath(base, model.source).generic_string()},
                   {"bones", model.bones},
                   {"morphs", model.morphs},
                   {"materials", model.materials},
                   {"materialAnnotations", model.materialAnnotations},
                   {"motionOrder", model.motionOrder},
                   {"deformOrder", model.deformOrder},
                   {"postprocessOrder", model.postprocessOrder},
                   {"rasterOrder", model.rasterOrder},
                   {"cloneCount", model.cloneCount},
                   {"visible", model.visible}};
        if (model.upstreamId.has_value())
            state["upstreamId"] = *model.upstreamId;
        native["modelState"].push_back(std::move(state));
    }
    root["mikumikudesu"] = std::move(native);

    auto temporary = absolute;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("cannot write project: " + temporary.string());
        output << "[MikuMikuDayo]\n" << root.dump(2) << "\n[BinaryDayo]\n";
        auto payload = project.embeddedVmdayo;
        if (payload.empty()) {
            std::vector<VmdMotion> motions = project.embeddedMotions;
            if (motions.empty() && project.embeddedMotion) {
                VmdMotion camera;
                camera.modelName = "Camera/Light";
                camera.cameras = project.embeddedMotion->cameras;
                camera.lights = project.embeddedMotion->lights;
                camera.shadows = project.embeddedMotion->shadows;
                auto model = *project.embeddedMotion;
                model.cameras.clear();
                model.lights.clear();
                model.shadows.clear();
                motions = {std::move(camera), std::move(model)};
            }
            const auto expected = modelAssets.size() + 1U;
            if (motions.empty())
                motions.emplace_back();
            motions.resize(expected);
            for (std::size_t index = 0; index < motions.size(); ++index) {
                VmdayoDocument document;
                document.type = index == 0 ? 1 : 0;
                document.modelName =
                    index == 0 && motions[index].modelName.empty() ? "Camera/Light" : motions[index].modelName;
                document.motion = toMotionDocument(motions[index]);
                for (std::size_t modelIndex = 0; modelIndex < modelAssets.size(); ++modelIndex) {
                    document.modelDictionary[static_cast<std::int32_t>(modelIndex + 1U)] =
                        portablePath(base, modelAssets[modelIndex]->path).generic_string();
                }
                auto subset = serializeVmdayoSubset(document);
                payload.insert(payload.end(), subset.begin(), subset.end());
            }
        }
        if (!payload.empty()) {
            output.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        }
        output.flush();
        if (!output)
            throw std::runtime_error("failed while writing project: " + temporary.string());
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
