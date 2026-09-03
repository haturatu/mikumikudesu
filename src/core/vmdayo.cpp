#include "core/vmdayo.hpp"
#include "core/editor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace dayo::core {
namespace {

using Json = nlohmann::json;

Float3 readFloat3(const Json& value) {
    Float3 result{};
    if (!value.is_array())
        return result;
    for (std::size_t i = 0; i < result.size() && i < value.size(); ++i)
        result[i] = value.at(i).get<float>();
    return result;
}

Float4 readFloat4(const Json& value) {
    Float4 result{0.0F, 0.0F, 0.0F, 1.0F};
    if (!value.is_array())
        return result;
    for (std::size_t i = 0; i < result.size() && i < value.size(); ++i)
        result[i] = value.at(i).get<float>();
    return result;
}

template <std::size_t N> std::array<std::uint8_t, N> readBytes(const Json& value) {
    std::array<std::uint8_t, N> result{};
    if (!value.is_array())
        return result;
    for (std::size_t i = 0; i < result.size() && i < value.size(); ++i) {
        result[i] = static_cast<std::uint8_t>(std::clamp(value.at(i).get<int>(), 0, 255));
    }
    return result;
}

MotionDocument readMotion(const Json& value) {
    MotionDocument result;
    if (!value.is_object())
        return result;
    result.interpolation = static_cast<InterpolationMode>(std::clamp(value.value("interpolation", 1), 0, 2));
    if (const auto bones = value.find("bones"); bones != value.end() && bones->is_array()) {
        for (const auto& item : *bones) {
            if (!item.is_object())
                continue;
            result.bones.push_back({item.value("name", ""), item.value("frame", 0U),
                                    readFloat3(item.value("translation", Json::array())),
                                    readFloat4(item.value("rotation", Json::array())),
                                    readBytes<64>(item.value("interpolation", Json::array())),
                                    item.value("physics", true), readBytes<4>(item.value("methods", Json::array()))});
        }
    }
    if (const auto morphs = value.find("morphs"); morphs != value.end() && morphs->is_array()) {
        for (const auto& item : *morphs)
            if (item.is_object())
                result.morphs.push_back({item.value("name", ""), item.value("frame", 0U), item.value("weight", 0.0F)});
    }
    if (const auto values = value.find("cameras"); values != value.end() && values->is_array()) {
        for (const auto& item : *values)
            if (item.is_object()) {
                VmdCameraKey key;
                key.frame = item.value("frame", 0U);
                key.distance = item.value("distance", 0.0F);
                key.position = readFloat3(item.value("position", Json::array()));
                key.rotation = readFloat3(item.value("rotation", Json::array()));
                key.interpolation = readBytes<24>(item.value("interpolation", Json::array()));
                key.viewAngle = item.value("viewAngle", 30.0F);
                key.perspective = item.value("perspective", true);
                key.parentModel = item.value("parentModel", -1);
                key.parentBone = item.value("parentBone", -1);
                key.parentBoneName = item.value("parentBoneName", "");
                key.methods = readBytes<6>(item.value("methods", Json::array()));
                result.cameras.push_back(key);
            }
    }
    if (const auto values = value.find("lights"); values != value.end() && values->is_array()) {
        for (const auto& item : *values)
            if (item.is_object())
                result.lights.push_back({item.value("frame", 0U), readFloat3(item.value("color", Json::array())),
                                         readFloat3(item.value("position", Json::array()))});
    }
    if (const auto values = value.find("shadows"); values != value.end() && values->is_array()) {
        for (const auto& item : *values)
            if (item.is_object())
                result.shadows.push_back({item.value("frame", 0U), static_cast<std::uint8_t>(item.value("mode", 0)),
                                          item.value("distance", 0.0F)});
    }
    if (const auto values = value.find("ik"); values != value.end() && values->is_array()) {
        for (const auto& item : *values)
            if (item.is_object()) {
                VmdIkKey key{item.value("frame", 0U), item.value("visible", true), {}};
                if (const auto states = item.find("states"); states != item.end() && states->is_array()) {
                    for (const auto& state : *states)
                        if (state.is_object())
                            key.states.push_back({state.value("name", ""), state.value("enabled", true)});
                }
                result.ik.push_back(std::move(key));
            }
    }
    if (const auto values = value.find("externalParents"); values != value.end() && values->is_array()) {
        for (const auto& item : *values)
            if (item.is_object())
                result.externalParents.push_back({item.value("frame", 0U), item.value("parentModel", -1),
                                                  item.value("parentBone", ""), item.value("childBone", "")});
    }
    if (const auto values = value.find("gravity"); values != value.end() && values->is_array()) {
        for (const auto& item : *values)
            if (item.is_object())
                result.gravity.push_back({item.value("frame", 0U), item.value("strength", 98.0F),
                                          readFloat3(item.value("direction", Json::array())),
                                          item.value("noiseAmplitude", 0.0F), item.value("noiseFrequency", 0.0F)});
    }
    MotionEditor::normalize(result);
    return result;
}

constexpr std::array<std::uint8_t, 8> vmdayoSignature{'V', 'M', 'D', 'A', 'Y', 'O', '!', 0};

class BinaryReader {
  public:
    explicit BinaryReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    template <typename T> T value(std::string_view field) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (cursor_ + sizeof(T) > bytes_.size())
            fail(field);
        T result{};
        std::memcpy(&result, bytes_.data() + cursor_, sizeof(T));
        cursor_ += sizeof(T);
        return result;
    }

    template <std::size_t N> std::array<std::uint8_t, N> byteArray(std::string_view field) {
        std::array<std::uint8_t, N> result{};
        if (cursor_ + N > bytes_.size())
            fail(field);
        std::copy_n(bytes_.data() + cursor_, N, result.data());
        cursor_ += N;
        return result;
    }

    std::string text(std::string_view field) {
        const auto count = value<std::int32_t>(field);
        if (count < 0 || count > 16 * 1024 * 1024 || cursor_ + static_cast<std::size_t>(count) > bytes_.size())
            fail(field);
        std::string result(reinterpret_cast<const char*>(bytes_.data() + cursor_), static_cast<std::size_t>(count));
        cursor_ += static_cast<std::size_t>(count);
        return result;
    }

    std::int32_t count(std::string_view field) {
        const auto result = value<std::int32_t>(field);
        if (result < 0 || result > 10'000'000)
            fail(field);
        return result;
    }

    [[nodiscard]] bool nextIsSignature() const noexcept {
        return cursor_ + vmdayoSignature.size() <= bytes_.size() &&
               std::equal(vmdayoSignature.begin(), vmdayoSignature.end(),
                          bytes_.begin() + static_cast<std::ptrdiff_t>(cursor_));
    }

    [[nodiscard]] std::size_t cursor() const noexcept {
        return cursor_;
    }

  private:
    [[noreturn]] static void fail(std::string_view field) {
        throw std::runtime_error("invalid VMdayo " + std::string(field));
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t cursor_{};
};

class BinaryWriter {
  public:
    template <typename T> void value(const T& item) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* begin = reinterpret_cast<const std::uint8_t*>(std::addressof(item));
        bytes_.insert(bytes_.end(), begin, begin + sizeof(T));
    }

    void text(std::string_view item) {
        if (item.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            throw std::length_error("VMdayo string is too long");
        }
        value(static_cast<std::int32_t>(item.size()));
        bytes_.insert(bytes_.end(), item.begin(), item.end());
    }

    [[nodiscard]] std::vector<std::uint8_t> take() && {
        return std::move(bytes_);
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

void readHeader(BinaryReader& reader, VmdayoDocument& document) {
    const auto signature = reader.byteArray<8>("signature");
    if (signature != vmdayoSignature)
        throw std::runtime_error("invalid VMdayo signature");
    document.version = reader.value<std::int32_t>("version");
    if (document.version < 3 || document.version > 3)
        throw std::runtime_error("unsupported VMdayo version");
    document.type = reader.value<std::int32_t>("type");
    document.modelName = reader.text("model name");
    for (std::int32_t i = 0, count = reader.count("model dictionary count"); i < count; ++i) {
        const auto id = reader.value<std::int32_t>("model id");
        document.modelDictionary[id] = reader.text("model dictionary name");
    }
    for (std::int32_t i = 0, count = reader.count("metadata count"); i < count; ++i) {
        auto key = reader.text("metadata key");
        document.metadata[std::move(key)] = reader.text("metadata value");
    }
}

void writeHeader(BinaryWriter& writer, const VmdayoDocument& document) {
    writer.value(vmdayoSignature);
    writer.value(std::int32_t{3});
    const bool cameraOnly =
        document.motion.bones.empty() && document.motion.morphs.empty() &&
        (!document.motion.cameras.empty() || !document.motion.lights.empty() || !document.motion.shadows.empty());
    writer.value(static_cast<std::int32_t>(document.type != 0 ? document.type : (cameraOnly ? 1 : 0)));
    writer.text(document.modelName.empty() && cameraOnly ? "Camera/Light" : document.modelName);
    writer.value(static_cast<std::int32_t>(document.modelDictionary.size()));
    for (const auto& [id, name] : document.modelDictionary) {
        writer.value(id);
        writer.text(name);
    }
    writer.value(static_cast<std::int32_t>(document.metadata.size()));
    for (const auto& [key, value] : document.metadata) {
        writer.text(key);
        writer.text(value);
    }
}

MotionDocument readSubset(BinaryReader& reader) {
    MotionDocument motion;
    for (std::int32_t i = 0, count = reader.count("bone count"); i < count; ++i) {
        VmdBoneKey key;
        key.name = reader.text("bone name");
        key.frame = static_cast<std::uint32_t>(reader.value<std::int32_t>("bone frame"));
        key.translation = reader.value<Float3>("bone translation");
        key.rotation = reader.value<Float4>("bone rotation");
        const auto internal = reader.byteArray<16>("bone interpolation");
        for (std::size_t channel = 0; channel < 4; ++channel)
            for (std::size_t point = 0; point < 4; ++point) {
                key.interpolation[channel + point * 4] = internal[channel * 4 + point];
            }
        key.physics = reader.value<std::uint8_t>("bone physics") != 0;
        key.methods = reader.byteArray<4>("bone methods");
        motion.bones.push_back(std::move(key));
    }
    for (std::int32_t i = 0, count = reader.count("morph count"); i < count; ++i) {
        motion.morphs.push_back({reader.text("morph name"),
                                 static_cast<std::uint32_t>(reader.value<std::int32_t>("morph frame")),
                                 reader.value<float>("morph weight")});
    }
    for (std::int32_t i = 0, count = reader.count("camera count"); i < count; ++i) {
        VmdCameraKey key;
        key.frame = static_cast<std::uint32_t>(reader.value<std::int32_t>("camera frame"));
        key.distance = reader.value<float>("camera distance");
        key.position = reader.value<Float3>("camera target");
        key.rotation = reader.value<Float3>("camera rotation");
        key.interpolation = reader.byteArray<24>("camera interpolation");
        key.viewAngle = reader.value<float>("camera view angle");
        key.perspective = reader.value<std::uint8_t>("camera perspective") != 0;
        key.parentModel = reader.value<std::int32_t>("camera parent model");
        key.parentBone = reader.value<std::int32_t>("camera parent bone");
        key.parentBoneName = reader.text("camera parent bone name");
        key.methods = reader.byteArray<6>("camera methods");
        motion.cameras.push_back(std::move(key));
    }
    for (std::int32_t i = 0, count = reader.count("light count"); i < count; ++i) {
        motion.lights.push_back({static_cast<std::uint32_t>(reader.value<std::int32_t>("light frame")),
                                 reader.value<Float3>("light color"), reader.value<Float3>("light direction")});
    }
    for (std::int32_t i = 0, count = reader.count("shadow count"); i < count; ++i) {
        motion.shadows.push_back({static_cast<std::uint32_t>(reader.value<std::int32_t>("shadow frame")),
                                  reader.value<std::uint8_t>("shadow mode"), reader.value<float>("shadow distance")});
    }
    for (std::int32_t i = 0, count = reader.count("extra count"); i < count; ++i) {
        const auto frame = static_cast<std::uint32_t>(reader.value<std::int32_t>("extra frame"));
        VmdIkKey key{frame, reader.value<std::uint8_t>("visibility") != 0, {}};
        for (std::int32_t j = 0, states = reader.count("IK count"); j < states; ++j) {
            key.states.push_back({reader.text("IK name"), reader.value<std::uint8_t>("IK enabled") != 0});
        }
        const auto parents = reader.count("external parent count");
        for (std::int32_t j = 0; j < parents; ++j) {
            motion.externalParents.push_back({frame, reader.value<std::int32_t>("external parent model"),
                                              reader.text("external parent bone"), reader.text("external child bone")});
        }
        if (parents == 0 || !key.visible || !key.states.empty())
            motion.ik.push_back(std::move(key));
    }
    for (std::int32_t i = 0, count = reader.count("gravity count"); i < count; ++i) {
        motion.gravity.push_back({static_cast<std::uint32_t>(reader.value<std::int32_t>("gravity frame")),
                                  reader.value<float>("gravity strength"), reader.value<Float3>("gravity direction"),
                                  reader.value<float>("gravity noise amplitude"),
                                  reader.value<float>("gravity noise frequency")});
    }
    if (std::ranges::any_of(motion.bones,
                            [](const auto& key) {
                                return std::ranges::any_of(key.methods, [](auto method) { return method == 1; });
                            }) ||
        std::ranges::any_of(motion.cameras, [](const auto& key) {
            return std::ranges::any_of(key.methods, [](auto method) { return method == 1; });
        })) {
        motion.interpolation = InterpolationMode::catmullRom;
    }
    MotionEditor::normalize(motion);
    return motion;
}

void writeSubset(BinaryWriter& writer, const MotionDocument& motion) {
    writer.value(static_cast<std::int32_t>(motion.bones.size()));
    for (const auto& key : motion.bones) {
        writer.text(key.name);
        writer.value(static_cast<std::int32_t>(key.frame));
        writer.value(key.translation);
        writer.value(key.rotation);
        std::array<std::uint8_t, 16> internal{};
        for (std::size_t channel = 0; channel < 4; ++channel)
            for (std::size_t point = 0; point < 4; ++point) {
                internal[channel * 4 + point] = key.interpolation[channel + point * 4];
            }
        writer.value(internal);
        writer.value(static_cast<std::uint8_t>(key.physics));
        auto methods = key.methods;
        if (motion.interpolation == InterpolationMode::catmullRom)
            methods.fill(1);
        writer.value(methods);
    }
    writer.value(static_cast<std::int32_t>(motion.morphs.size()));
    for (const auto& key : motion.morphs) {
        writer.text(key.name);
        writer.value(static_cast<std::int32_t>(key.frame));
        writer.value(key.weight);
    }
    writer.value(static_cast<std::int32_t>(motion.cameras.size()));
    for (const auto& key : motion.cameras) {
        writer.value(static_cast<std::int32_t>(key.frame));
        writer.value(key.distance);
        writer.value(key.position);
        writer.value(key.rotation);
        writer.value(key.interpolation);
        writer.value(key.viewAngle);
        writer.value(static_cast<std::uint8_t>(key.perspective));
        writer.value(key.parentModel);
        writer.value(key.parentBone);
        writer.text(key.parentBoneName);
        auto methods = key.methods;
        if (motion.interpolation == InterpolationMode::catmullRom)
            methods.fill(1);
        writer.value(methods);
    }
    writer.value(static_cast<std::int32_t>(motion.lights.size()));
    for (const auto& key : motion.lights) {
        writer.value(static_cast<std::int32_t>(key.frame));
        writer.value(key.color);
        writer.value(key.position);
    }
    writer.value(static_cast<std::int32_t>(motion.shadows.size()));
    for (const auto& key : motion.shadows) {
        writer.value(static_cast<std::int32_t>(key.frame));
        writer.value(key.mode);
        writer.value(key.distance);
    }

    std::vector<std::uint32_t> extraFrames;
    extraFrames.reserve(motion.ik.size() + motion.externalParents.size());
    for (const auto& key : motion.ik)
        extraFrames.push_back(key.frame);
    for (const auto& key : motion.externalParents)
        extraFrames.push_back(key.frame);
    std::ranges::sort(extraFrames);
    extraFrames.erase(std::unique(extraFrames.begin(), extraFrames.end()), extraFrames.end());
    writer.value(static_cast<std::int32_t>(extraFrames.size()));
    for (const auto frame : extraFrames) {
        const auto ik = std::ranges::find(motion.ik, frame, &VmdIkKey::frame);
        writer.value(static_cast<std::int32_t>(frame));
        writer.value(static_cast<std::uint8_t>(ik == motion.ik.end() || ik->visible));
        writer.value(static_cast<std::int32_t>(ik == motion.ik.end() ? 0 : ik->states.size()));
        if (ik != motion.ik.end())
            for (const auto& state : ik->states) {
                writer.text(state.name);
                writer.value(static_cast<std::uint8_t>(state.enabled));
            }
        const auto parentCount = std::ranges::count(motion.externalParents, frame, &VmdayoExternalParentKey::frame);
        writer.value(static_cast<std::int32_t>(parentCount));
        for (const auto& parent : motion.externalParents)
            if (parent.frame == frame) {
                writer.value(parent.parentModel);
                writer.text(parent.parentBone);
                writer.text(parent.childBone);
            }
    }
    writer.value(static_cast<std::int32_t>(motion.gravity.size()));
    for (const auto& key : motion.gravity) {
        writer.value(static_cast<std::int32_t>(key.frame));
        writer.value(key.strength);
        writer.value(key.direction);
        writer.value(key.noiseAmplitude);
        writer.value(key.noiseFrequency);
    }
}

std::vector<std::uint8_t> binarySubset(const VmdayoDocument& document, bool duplicateHeader) {
    BinaryWriter writer;
    writeHeader(writer, document);
    if (duplicateHeader)
        writeHeader(writer, document);
    writeSubset(writer, document.motion);
    return std::move(writer).take();
}

} // namespace

VmdayoDocument loadVmdayo(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open VMdayo file: " + path.string());
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return parseVmdayo(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

VmdayoDocument parseVmdayo(std::span<const std::uint8_t> bytes) {
    VmdayoDocument result;
    if (bytes.empty())
        return result;
    try {
        if (bytes.size() >= vmdayoSignature.size() &&
            std::equal(vmdayoSignature.begin(), vmdayoSignature.end(), bytes.begin())) {
            BinaryReader reader(bytes);
            readHeader(reader, result);
            // Standalone .vmdayo repeats the header: once for routing and once
            // as part of WriteSubset. A .dayo v3 embedded subset has one.
            if (reader.nextIsSignature())
                readHeader(reader, result);
            result.motion = readSubset(reader);
            result.motion.modelName = result.modelName;
            return result;
        }
        const auto root = Json::parse(bytes.begin(), bytes.end());
        result.version = root.value("version", 3);
        result.modelName = root.value("model", "");
        result.motion = readMotion(root.value("motion", Json::object()));
        return result;
    } catch (const std::exception&) {
        result.opaque = {bytes.begin(), bytes.end()};
        return result;
    }
}

std::vector<VmdayoDocument> parseVmdayoSubsets(std::span<const std::uint8_t> bytes, std::size_t count) {
    std::vector<VmdayoDocument> result;
    result.reserve(count);
    BinaryReader reader(bytes);
    for (std::size_t index = 0; index < count; ++index) {
        VmdayoDocument document;
        readHeader(reader, document);
        document.motion = readSubset(reader);
        document.motion.modelName = document.modelName;
        result.push_back(std::move(document));
    }
    if (reader.cursor() != bytes.size()) {
        throw std::runtime_error("unexpected trailing VMdayo subset data");
    }
    return result;
}

std::vector<std::uint8_t> serializeVmdayo(const VmdayoDocument& document) {
    if (!document.opaque.empty())
        return document.opaque;
    return binarySubset(document, true);
}

std::vector<std::uint8_t> serializeVmdayoSubset(const VmdayoDocument& document) {
    if (!document.opaque.empty())
        return document.opaque;
    return binarySubset(document, false);
}

void saveVmdayo(const std::filesystem::path& path, const VmdayoDocument& document) {
    if (path.empty())
        throw std::invalid_argument("VMdayo path is empty");
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    std::filesystem::create_directories(absolute.parent_path());
    auto temporary = absolute;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("cannot write VMdayo file: " + temporary.string());
        const auto bytes = serializeVmdayo(document);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output)
            throw std::runtime_error("failed while writing VMdayo file: " + temporary.string());
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
        throw std::runtime_error("cannot replace VMdayo file: " + error.message());
    }
}

} // namespace dayo::core
