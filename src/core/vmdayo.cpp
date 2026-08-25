#include "core/vmdayo.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>
#include <stdexcept>

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

Json motionJson(const MotionDocument& motion) {
    Json result = Json::object();
    result["interpolation"] = static_cast<int>(motion.interpolation);
    result["bones"] = Json::array();
    for (const auto& key : motion.bones) {
        result["bones"].push_back({ { "name", key.name }, { "frame", key.frame },
            { "translation", float3(key.translation) }, { "rotation", float4(key.rotation) } });
    }
    result["morphs"] = Json::array();
    for (const auto& key : motion.morphs)
        result["morphs"].push_back({ { "name", key.name }, { "frame", key.frame }, { "weight", key.weight } });
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
                readFloat4(item.value("rotation", Json::array())) });
        }
    }
    if (const auto morphs = value.find("morphs"); morphs != value.end() && morphs->is_array()) {
        for (const auto& item : *morphs) if (item.is_object())
            result.morphs.push_back({ item.value("name", ""), item.value("frame", 0U), item.value("weight", 0.0F) });
    }
    return result;
}

} // namespace

VmdayoDocument loadVmdayo(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open VMdayo file: " + path.string());
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    VmdayoDocument result;
    if (bytes.empty()) return result;
    try {
        const auto root = Json::parse(bytes);
        result.version = root.value("version", 1);
        result.modelName = root.value("model", "");
        result.motion = readMotion(root.value("motion", Json::object()));
        return result;
    } catch (const Json::exception&) {
        result.opaque.assign(bytes.begin(), bytes.end());
        return result;
    }
}

void saveVmdayo(const std::filesystem::path& path, const VmdayoDocument& document) {
    if (path.empty()) throw std::invalid_argument("VMdayo path is empty");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write VMdayo file: " + path.string());
    if (!document.opaque.empty()) {
        output.write(reinterpret_cast<const char*>(document.opaque.data()),
                     static_cast<std::streamsize>(document.opaque.size()));
        return;
    }
    const Json root { { "version", document.version }, { "model", document.modelName },
                      { "motion", motionJson(document.motion) } };
    output << root.dump(2) << '\n';
    if (!output) throw std::runtime_error("failed while writing VMdayo file: " + path.string());
}

} // namespace dayo::core
