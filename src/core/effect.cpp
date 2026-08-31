#include "core/effect.hpp"

#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#if DAYO_HAS_JSONNET
extern "C" {
#include <libjsonnet.h>
}
#include <nlohmann/json.hpp>
#endif

namespace dayo::core {
namespace {

#if DAYO_HAS_JSONNET
EffectPassType passType(std::string_view value) {
    if (value == "rasterizer")
        return EffectPassType::rasterizer;
    if (value == "postprocess")
        return EffectPassType::postprocess;
    if (value == "compute")
        return EffectPassType::compute;
    if (value == "raytracing")
        return EffectPassType::raytracing;
    return EffectPassType::unknown;
}
#endif

#if DAYO_HAS_JSONNET
std::string evaluateJsonnet(const std::filesystem::path& path, std::string_view source) {
    auto* vm = jsonnet_make();
    if (vm == nullptr)
        throw std::runtime_error("cannot create Jsonnet VM");
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
    if (error != 0)
        throw std::runtime_error("Jsonnet evaluation failed: " + result);
    return result;
}

std::vector<std::string> strings(const nlohmann::json& parent, std::string_view name) {
    std::vector<std::string> result;
    const auto found = parent.find(name);
    if (found == parent.end() || !found->is_array())
        return result;
    for (const auto& value : *found)
        if (value.is_string())
            result.push_back(value.get<std::string>());
    return result;
}

std::vector<EffectAttachment> attachments(const nlohmann::json& parent, std::string_view name) {
    std::vector<EffectAttachment> result;
    const auto found = parent.find(name);
    if (found == parent.end() || !found->is_array())
        return result;
    for (const auto& value : *found) {
        if (value.is_string())
            result.push_back({value.get<std::string>(), false});
        else if (value.is_object())
            result.push_back({value.value("name", ""), value.value("clear", false)});
    }
    return result;
}
#endif

} // namespace

const MaterialValue* MaterialParameterBlock::find(std::string_view name) const noexcept {
    const auto found = values_.find(std::string(name));
    return found == values_.end() ? nullptr : std::addressof(found->second);
}

bool MaterialParameterBlock::erase(std::string_view name) {
    return values_.erase(std::string(name)) != 0;
}

EffectGraph loadEffectGraph(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open effect: " + path.string());
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
    // Subayai keeps shared HLSL declarations before [YRZFX]. Preserve both
    // HLSL regions so runtime shader compilation sees the complete source.
    graph.hlsl = source.substr(0, jsonStart);
    graph.hlsl += source.substr(hlslStart + hlslMarker.size());
#if DAYO_HAS_JSONNET
    const auto jsonText =
        evaluateJsonnet(path, std::string_view(source).substr(jsonStart + jsonMarker.size(),
                                                              hlslStart - jsonStart - jsonMarker.size()));
    const auto root = nlohmann::json::parse(jsonText);
    const auto found = root.find("fx");
    if (found == root.end() || !found->is_object())
        throw std::runtime_error("effect has no fx object");
    const auto& fx = *found;
    graph.category = fx.value("category", "");
    if (const auto values = fx.find("textures"); values != fx.end() && values->is_array()) {
        for (const auto& value : *values) {
            EffectTexture texture;
            texture.name = value.value("name", "");
            texture.format = value.value("format", "");
            texture.view = value.value("view", "");
            texture.conditions = strings(value, "conditions");
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
        for (const auto& value : *values)
            graph.samplers.push_back({value.value("name", ""), value.value("filter", ""),
                                      value.value("addressU", "WRAP"), value.value("addressV", "WRAP")});
    }
    if (const auto values = fx.find("controllers"); values != fx.end() && values->is_array()) {
        for (const auto& value : *values)
            if (value.is_object())
                graph.controllers.push_back({value.value("name", ""), value.value("controllerName", ""),
                                             value.value("item", ""), value.value("type", "")});
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
            if (const auto groups = value.find("hitGroup"); groups != value.end() && groups->is_array()) {
                for (const auto& group : *groups)
                    if (group.is_object())
                        pass.hitGroups.push_back({group.value("type", ""), group.value("closestHit", ""),
                                                  group.value("anyHit", ""), group.value("intersection", "")});
            }
            pass.macros = strings(value, "macros");
            pass.conditions = strings(value, "conditions");
            pass.renderTargets = attachments(value, "RTV");
            pass.unorderedAccess = attachments(value, "UAV");
            if (const auto depth = value.find("DSV"); depth != value.end()) {
                if (depth->is_string())
                    pass.depth.name = depth->get<std::string>();
                else if (depth->is_object())
                    pass.depth = {depth->value("name", ""), depth->value("clear", false)};
            }
            if (const auto size = value.find("outputSize"); size != value.end() && size->is_object()) {
                if (const auto ratio = size->find("ratio"); ratio != size->end() && ratio->is_object()) {
                    pass.outputWidthRatio = ratio->value("x", 1.0F);
                    pass.outputHeightRatio = ratio->value("y", 1.0F);
                }
            }
            pass.maxPayloadSize = value.value("maxPayloadSize", 0U);
            pass.maxAttributeSize = value.value("maxAttributeSize", 0U);
            pass.maxRecursionDepth = value.value("maxRecursionDepth", 1U);
            graph.passes.push_back(std::move(pass));
        }
    }
    if (const auto code = fx.find("code"); code != fx.end() && code->is_array()) {
        for (const auto& line : *code)
            if (line.is_string())
                graph.generatedCode += line.get<std::string>() + '\n';
    }
#else
    throw std::runtime_error("Jsonnet support was not built");
#endif
    return graph;
}

CompiledEffect compileEffectGraph(const EffectGraph& graph) {
    CompiledEffect result;
    result.source = graph;
    std::unordered_map<std::string, bool> writers;
    for (const auto& pass : graph.passes) {
        CompiledPass compiled{
            .name = pass.name,
            .type = pass.type,
            .resources = {},
            .barriers = {},
            .vertexShader = pass.vertexShader,
            .pixelShader = pass.pixelShader,
            .computeShader = pass.computeShader,
            .rayGenerationShader = pass.rayGenerationShader,
            .missShaders = pass.missShaders,
            .hitGroups = pass.hitGroups,
            .conditions = pass.conditions,
            .outputWidthRatio = pass.outputWidthRatio,
            .outputHeightRatio = pass.outputHeightRatio,
            .maxPayloadSize = pass.maxPayloadSize,
            .maxAttributeSize = pass.maxAttributeSize,
            .maxRecursionDepth = pass.maxRecursionDepth,
        };
        for (const auto& input : pass.renderTargets) {
            if (input.name.empty())
                continue;
            compiled.resources.push_back({input.name, true});
            if (writers.contains(input.name))
                compiled.barriers.push_back("write-after-write:" + input.name);
            writers[input.name] = true;
        }
        for (const auto& input : pass.unorderedAccess) {
            if (input.name.empty())
                continue;
            compiled.resources.push_back({input.name, true});
            if (writers.contains(input.name))
                compiled.barriers.push_back("uav:" + input.name);
            writers[input.name] = true;
        }
        if (!pass.depth.name.empty()) {
            compiled.resources.push_back({pass.depth.name, pass.depth.clear});
            if (writers.contains(pass.depth.name))
                compiled.barriers.push_back("depth:" + pass.depth.name);
            writers[pass.depth.name] = true;
        }
        result.passes.push_back(std::move(compiled));
    }
    return result;
}

EffectExecutionStats EffectExecutor::execute(const CompiledEffect& effect, const PassCallback& callback) const {
    EffectExecutionStats stats;
    for (const auto& pass : effect.passes) {
        stats.barriers += pass.barriers.size();
        switch (pass.type) {
        case EffectPassType::rasterizer:
        case EffectPassType::postprocess:
            ++stats.rasterPasses;
            break;
        case EffectPassType::compute:
            ++stats.computePasses;
            break;
        case EffectPassType::raytracing:
            ++stats.rayTracingPasses;
            break;
        case EffectPassType::unknown:
            break;
        }
        if (callback)
            callback(pass);
    }
    return stats;
}

EffectHotReloader::EffectHotReloader(std::filesystem::path path) : path_(std::move(path)) {}

bool EffectHotReloader::poll(std::string* error) {
    std::error_code statusError;
    const auto timestamp = std::filesystem::last_write_time(path_, statusError);
    if (statusError) {
        if (error != nullptr)
            *error = statusError.message();
        return false;
    }
    if (graph_ && timestamp == timestamp_)
        return false;
    try {
        auto candidate = loadEffectGraph(path_);
        // Compile before committing the candidate so broken Jsonnet/HLSL does
        // not tear down the last known-good effect.
        static_cast<void>(compileEffectGraph(candidate));
        graph_ = std::move(candidate);
        timestamp_ = timestamp;
        dirty_ = true;
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = exception.what();
        return false;
    }
}

const char* toString(EffectPassType type) noexcept {
    switch (type) {
    case EffectPassType::rasterizer:
        return "rasterizer";
    case EffectPassType::postprocess:
        return "postprocess";
    case EffectPassType::compute:
        return "compute";
    case EffectPassType::raytracing:
        return "raytracing";
    case EffectPassType::unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace dayo::core
