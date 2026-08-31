#include "core/vmdayo.hpp"
#include "core/editor.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <system_error>

namespace dayo::core {
namespace {

using Json = nlohmann::json;

Json float3(const Float3& value) { return { value[0], value[1], value[2] }; }
Json float4(const Float4& value) { return { value[0], value[1], value[2], value[3] }; }

Float3 readFloat3(const Json& value) {
    Float3 result {};
    if (!value.is_array()) return result;
    for (std::size_t i = 0; i < result.size() && i < value.size(); ++i) result[i] = value.at(i).get<float>();
    return result;
}

Float4 readFloat4(const Json& value) {
    Float4 result { 0.0F, 0.0F, 0.0F, 1.0F };
    if (!value.is_array()) return result;
    for (std::size_t i = 0; i < result.size() && i < value.size(); ++i) result[i] = value.at(i).get<float>();
    return result;
}

template <std::size_t N>
std::array<std::uint8_t, N> readBytes(const Json& value) {
    std::array<std::uint8_t, N> result {};
    if (!value.is_array()) return result;
    for (std::size_t i = 0; i < result.size() && i < value.size(); ++i) {
        result[i] = static_cast<std::uint8_t>(std::clamp(value.at(i).get<int>(), 0, 255));
    }
    return result;
}

Json motionJson(const MotionDocument& motion) {
    Json result = Json::object();
    result["interpolation"] = static_cast<int>(motion.interpolation);
    result["bones"] = Json::array();
    for (const auto& key : motion.bones) {
        result["bones"].push_back({ { "name", key.name }, { "frame", key.frame },
            { "translation", float3(key.translation) }, { "rotation", float4(key.rotation) },
            { "interpolation", key.interpolation }, { "physics", key.physics } });
    }
    result["morphs"] = Json::array();
    for (const auto& key : motion.morphs)
        result["morphs"].push_back({ { "name", key.name }, { "frame", key.frame }, { "weight", key.weight } });
    result["cameras"] = Json::array();
    for (const auto& key : motion.cameras) result["cameras"].push_back({
        { "frame", key.frame }, { "distance", key.distance }, { "position", float3(key.position) },
        { "rotation", float3(key.rotation) }, { "interpolation", key.interpolation },
        { "viewAngle", key.viewAngle }, { "perspective", key.perspective },
        { "parentModel", key.parentModel }, { "parentBone", key.parentBone } });
    result["lights"] = Json::array();
    for (const auto& key : motion.lights) result["lights"].push_back({
        { "frame", key.frame }, { "color", float3(key.color) }, { "position", float3(key.position) } });
    result["shadows"] = Json::array();
    for (const auto& key : motion.shadows) result["shadows"].push_back({
        { "frame", key.frame }, { "mode", key.mode }, { "distance", key.distance } });
    result["ik"] = Json::array();
    for (const auto& key : motion.ik) {
        Json states = Json::array();
        for (const auto& state : key.states) states.push_back({ { "name", state.name }, { "enabled", state.enabled } });
        result["ik"].push_back({ { "frame", key.frame }, { "visible", key.visible }, { "states", std::move(states) } });
    }
    result["externalParents"] = Json::array();
    for (const auto& key : motion.externalParents) result["externalParents"].push_back({
        { "frame", key.frame }, { "parentModel", key.parentModel },
        { "parentBone", key.parentBone }, { "childBone", key.childBone } });
    result["gravity"] = Json::array();
    for (const auto& key : motion.gravity) result["gravity"].push_back({
        { "frame", key.frame }, { "strength", key.strength }, { "direction", float3(key.direction) },
        { "noiseAmplitude", key.noiseAmplitude }, { "noiseFrequency", key.noiseFrequency } });
    return result;
}

MotionDocument readMotion(const Json& value) {
    MotionDocument result;
    if (!value.is_object()) return result;
    result.interpolation = static_cast<InterpolationMode>(std::clamp(value.value("interpolation", 1), 0, 2));
    if (const auto bones = value.find("bones"); bones != value.end() && bones->is_array()) {
        for (const auto& item : *bones) {
            if (!item.is_object()) continue;
            result.bones.push_back({ item.value("name", ""), item.value("frame", 0U),
                readFloat3(item.value("translation", Json::array())),
                readFloat4(item.value("rotation", Json::array())),
                readBytes<64>(item.value("interpolation", Json::array())),
                item.value("physics", true) });
        }
    }
    if (const auto morphs = value.find("morphs"); morphs != value.end() && morphs->is_array()) {
        for (const auto& item : *morphs) if (item.is_object())
            result.morphs.push_back({ item.value("name", ""), item.value("frame", 0U), item.value("weight", 0.0F) });
    }
    if (const auto values = value.find("cameras"); values != value.end() && values->is_array()) {
        for (const auto& item : *values) if (item.is_object()) {
            VmdCameraKey key;
            key.frame = item.value("frame", 0U);
            key.distance = item.value("distance", 0.0F);
            key.position = readFloat3(item.value("position", Json::array()));
            key.rotation = readFloat3(item.value("rotation", Json::array()));
            key.interpolation = readBytes<24>(item.value("interpolation", Json::array()));
            key.viewAngle = item.value("viewAngle", 30U);
            key.perspective = item.value("perspective", true);
            key.parentModel = item.value("parentModel", -1);
            key.parentBone = item.value("parentBone", -1);
            result.cameras.push_back(key);
        }
    }
    if (const auto values = value.find("lights"); values != value.end() && values->is_array()) {
        for (const auto& item : *values) if (item.is_object()) result.lights.push_back({
            item.value("frame", 0U), readFloat3(item.value("color", Json::array())),
            readFloat3(item.value("position", Json::array())) });
    }
    if (const auto values = value.find("shadows"); values != value.end() && values->is_array()) {
        for (const auto& item : *values) if (item.is_object()) result.shadows.push_back({
            item.value("frame", 0U), static_cast<std::uint8_t>(item.value("mode", 0)),
            item.value("distance", 0.0F) });
    }
    if (const auto values = value.find("ik"); values != value.end() && values->is_array()) {
        for (const auto& item : *values) if (item.is_object()) {
            VmdIkKey key { item.value("frame", 0U), item.value("visible", true), {} };
            if (const auto states = item.find("states"); states != item.end() && states->is_array()) {
                for (const auto& state : *states) if (state.is_object()) key.states.push_back({
                    state.value("name", ""), state.value("enabled", true) });
            }
            result.ik.push_back(std::move(key));
        }
    }
    if (const auto values = value.find("externalParents"); values != value.end() && values->is_array()) {
        for (const auto& item : *values) if (item.is_object()) result.externalParents.push_back({
            item.value("frame", 0U), item.value("parentModel", -1),
            item.value("parentBone", ""), item.value("childBone", "") });
    }
    if (const auto values = value.find("gravity"); values != value.end() && values->is_array()) {
        for (const auto& item : *values) if (item.is_object()) result.gravity.push_back({
            item.value("frame", 0U), item.value("strength", 98.0F),
            readFloat3(item.value("direction", Json::array())),
            item.value("noiseAmplitude", 0.0F), item.value("noiseFrequency", 0.0F) });
    }
    MotionEditor::normalize(result);
    return result;
}

} // namespace

VmdayoDocument loadVmdayo(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open VMdayo file: " + path.string());
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return parseVmdayo(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

VmdayoDocument parseVmdayo(std::span<const std::uint8_t> bytes) {
    VmdayoDocument result;
    if (bytes.empty()) return result;
    try {
        const auto root = Json::parse(bytes.begin(), bytes.end());
        result.version = root.value("version", 3);
        result.modelName = root.value("model", "");
        result.motion = readMotion(root.value("motion", Json::object()));
        return result;
    } catch (const Json::exception&) {
        result.opaque = { bytes.begin(), bytes.end() };
        return result;
    }
}

std::vector<std::uint8_t> serializeVmdayo(const VmdayoDocument& document) {
    if (!document.opaque.empty()) return document.opaque;
    const Json root { { "format", "MikuMikuDayo VMdayo" }, { "version", document.version },
                      { "model", document.modelName }, { "motion", motionJson(document.motion) } };
    const auto bytes = root.dump(2) + '\n';
    return { bytes.begin(), bytes.end() };
}

void saveVmdayo(const std::filesystem::path& path, const VmdayoDocument& document) {
    if (path.empty()) throw std::invalid_argument("VMdayo path is empty");
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    std::filesystem::create_directories(absolute.parent_path());
    auto temporary = absolute;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot write VMdayo file: " + temporary.string());
        const auto bytes = serializeVmdayo(document);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) throw std::runtime_error("failed while writing VMdayo file: " + temporary.string());
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
