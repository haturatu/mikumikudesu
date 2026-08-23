#include "core/effect.hpp"

#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string_view>

#if DAYO_HAS_JSONNET
extern "C" {
#include <libjsonnet.h>
}
#include <nlohmann/json.hpp>
#endif

namespace dayo::core {
namespace {

EffectPassType passType(std::string_view value) {
    if (value == "rasterizer") return EffectPassType::rasterizer;
    if (value == "postprocess") return EffectPassType::postprocess;
    if (value == "compute") return EffectPassType::compute;
    if (value == "raytracing") return EffectPassType::raytracing;
    return EffectPassType::unknown;
}

#if DAYO_HAS_JSONNET
std::string evaluateJsonnet(const std::filesystem::path& path, std::string_view source) {
    auto* vm = jsonnet_make();
    if (vm == nullptr) throw std::runtime_error("cannot create Jsonnet VM");
    const auto parent = path.parent_path().string();
    jsonnet_jpath_add(vm, parent.c_str());
    int error = 0;
    char* output = jsonnet_evaluate_snippet(vm, path.string().c_str(), std::string(source).c_str(), &error);
    if (output == nullptr) {
        jsonnet_destroy(vm);
        throw std::runtime_error("Jsonnet evaluation returned no output");
    }
    std::string result(output);
    jsonnet_realloc(vm, output, 0);
    jsonnet_destroy(vm);
    if (error != 0) throw std::runtime_error("Jsonnet evaluation failed: " + result);
    return result;
}

std::vector<std::string> strings(const nlohmann::json& parent, std::string_view name) {
    std::vector<std::string> result;
    const auto found = parent.find(name);
    if (found == parent.end() || !found->is_array()) return result;
    for (const auto& value : *found) if (value.is_string()) result.push_back(value.get<std::string>());
    return result;
}

std::vector<EffectAttachment> attachments(const nlohmann::json& parent, std::string_view name) {
    std::vector<EffectAttachment> result;
    const auto found = parent.find(name);
    if (found == parent.end() || !found->is_array()) return result;
    for (const auto& value : *found) {
        if (value.is_string()) result.push_back({ value.get<std::string>(), false });
        else if (value.is_object()) result.push_back({ value.value("name", ""), value.value("clear", false) });
    }
    return result;
}
#endif

} // namespace

EffectGraph loadEffectGraph(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open effect: " + path.string());
    const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    constexpr std::string_view jsonMarker = "[YRZFX]";
    constexpr std::string_view hlslMarker = "[HLSL]";
    const auto jsonStart = source.find(jsonMarker);
    const auto hlslStart = source.find(hlslMarker, jsonStart == std::string::npos ? 0 : jsonStart + jsonMarker.size());
    if (jsonStart == std::string::npos || hlslStart == std::string::npos || hlslStart <= jsonStart) {
        throw std::runtime_error("effect does not contain YRZFX/HLSL sections");
    }
    EffectGraph graph;
    graph.sourcePath = path;
    graph.hlsl = source.substr(hlslStart + hlslMarker.size());
#if DAYO_HAS_JSONNET
    const auto jsonText = evaluateJsonnet(path, std::string_view(source).substr(
        jsonStart + jsonMarker.size(), hlslStart - jsonStart - jsonMarker.size()));
    const auto root = nlohmann::json::parse(jsonText);
    const auto found = root.find("fx");
    if (found == root.end() || !found->is_object()) throw std::runtime_error("effect has no fx object");
    const auto& fx = *found;
    graph.category = fx.value("category", "");
    if (const auto values = fx.find("textures"); values != fx.end() && values->is_array()) {
        for (const auto& value : *values) {
            EffectTexture texture;
            texture.name = value.value("name", "");
            texture.format = value.value("format", "");
            texture.view = value.value("view", "");
            if (const auto size = value.find("size"); size != value.end() && size->is_object()) {
                if (const auto ratio = size->find("ratio"); ratio != size->end() && ratio->is_object()) {
                    texture.widthRatio = ratio->value("x", 1.0F);
                    texture.heightRatio = ratio->value("y", 1.0F);
                }
            }
            graph.textures.push_back(std::move(texture));
        }
    }
    if (const auto values = fx.find("samplers"); values != fx.end() && values->is_array()) {
        for (const auto& value : *values) graph.samplers.push_back({
            value.value("name", ""), value.value("filter", ""),
            value.value("addressU", "WRAP"), value.value("addressV", "WRAP") });
    }
    if (const auto values = fx.find("passes"); values != fx.end() && values->is_array()) {
        for (const auto& value : *values) {
            EffectPass pass;
            pass.name = value.value("name", "");
            pass.type = passType(value.value("type", ""));
            pass.vertexShader = value.value("vertexShader", "");
            pass.pixelShader = value.value("pixelShader", "");
            pass.computeShader = value.value("computeShader", "");
            pass.rayGenerationShader = value.value("raygenShader", "");
            pass.missShaders = strings(value, "missShader");
            pass.macros = strings(value, "macros");
            pass.conditions = strings(value, "conditions");
            pass.renderTargets = attachments(value, "RTV");
            pass.unorderedAccess = attachments(value, "UAV");
            if (const auto depth = value.find("DSV"); depth != value.end()) {
                if (depth->is_string()) pass.depth.name = depth->get<std::string>();
                else if (depth->is_object()) pass.depth = { depth->value("name", ""), depth->value("clear", false) };
            }
            graph.passes.push_back(std::move(pass));
        }
    }
    if (const auto code = fx.find("code"); code != fx.end() && code->is_array()) {
        for (const auto& line : *code) if (line.is_string()) graph.generatedCode += line.get<std::string>() + '\n';
    }
#else
    throw std::runtime_error("Jsonnet support was not built");
#endif
    return graph;
}

const char* toString(EffectPassType type) noexcept {
    switch (type) {
    case EffectPassType::rasterizer: return "rasterizer";
    case EffectPassType::postprocess: return "postprocess";
    case EffectPassType::compute: return "compute";
    case EffectPassType::raytracing: return "raytracing";
    case EffectPassType::unknown: return "unknown";
    }
    return "unknown";
}

} // namespace dayo::core
