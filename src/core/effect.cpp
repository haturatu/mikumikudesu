#include "core/effect.hpp"

#include "core/fx/fx_pass.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <ranges>
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
    if (value == "copy")
        return EffectPassType::copy;
    if (value == "clear" || value == "clearRtv" || value == "clearRTV" || value == "clearUav" || value == "clearUAV")
        return EffectPassType::clear;
    if (value == "mipmap" || value == "mipmapGen" || value == "mipmapgen")
        return EffectPassType::mipmap;
    if (value == "oidn")
        return EffectPassType::oidn;
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

std::string lower(std::string_view value) {
    std::string result(value);
    for (auto& character : result)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return result;
}

std::string compactKey(std::string_view value) {
    std::string result;
    for (const auto character : lower(value)) {
        if (character != '_' && character != '-')
            result.push_back(character);
    }
    return result;
}

std::uint32_t hlslElementSize(std::string_view type) {
    std::string key;
    key.reserve(type.size());
    for (const auto character : lower(type))
        if (!std::isspace(static_cast<unsigned char>(character)))
            key.push_back(character);

    std::uint32_t scalarSize = 0;
    std::size_t prefixSize = 0;
    for (const auto& [prefix, size] : std::array<std::pair<std::string_view, std::uint32_t>, 5>{
             {{"double", 8}, {"float", 4}, {"uint", 4}, {"int", 4}, {"bool", 4}}}) {
        if (key.starts_with(prefix)) {
            scalarSize = size;
            prefixSize = prefix.size();
            break;
        }
    }
    if (scalarSize == 0)
        return 0;

    const auto shape = std::string_view(key).substr(prefixSize);
    if (shape.empty())
        return scalarSize;
    const auto x = shape.find('x');
    const auto parseDimension = [](std::string_view text) -> std::uint32_t {
        if (text.empty())
            return 0;
        std::uint32_t result = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
        return error == std::errc{} && end == text.data() + text.size() ? result : 0;
    };
    const auto first = parseDimension(x == std::string_view::npos ? shape : shape.substr(0, x));
    if (first == 0 || first > 4)
        return 0;
    std::uint32_t components = first;
    if (x != std::string_view::npos) {
        const auto second = parseDimension(shape.substr(x + 1));
        if (second == 0 || second > 4)
            return 0;
        components *= second;
    }
    if (shape.find_first_not_of("0123456789x") != std::string_view::npos)
        return 0;
    const auto bytes = static_cast<std::uint64_t>(scalarSize) * components;
    return bytes <= std::numeric_limits<std::uint32_t>::max() ? static_cast<std::uint32_t>(bytes) : 0;
}

EffectClearValue clearValue(const nlohmann::json& value) {
    EffectClearValue result;
    if (!value.is_object())
        return result;
    result.color = {value.value("x", 0.0F), value.value("y", 0.0F), value.value("z", 0.0F), value.value("w", 0.0F)};
    result.depth = value.value("depth", 1.0F);
    result.stencil = value.value("stencil", 0U);
    return result;
}

EffectAttachment attachment(const nlohmann::json& value) {
    if (value.is_string())
        return {.name = value.get<std::string>(), .clear = false, .clearValue = {}};
    if (!value.is_object())
        return {};
    EffectAttachment result;
    result.name = value.value("name", "");
    result.clear = value.value("clear", false);
    if (const auto clear = value.find("value"); clear != value.end())
        result.clearValue = clearValue(*clear);
    if (const auto clear = value.find("clearValue"); clear != value.end())
        result.clearValue = clearValue(*clear);
    if (const auto depth = value.find("depth"); depth != value.end() && depth->is_number())
        result.clearValue.depth = depth->get<float>();
    if (const auto stencil = value.find("stencil"); stencil != value.end() && stencil->is_number_unsigned())
        result.clearValue.stencil = stencil->get<std::uint32_t>();
    return result;
}

std::vector<EffectAttachment> attachments(const nlohmann::json& parent, std::string_view name) {
    std::vector<EffectAttachment> result;
    const auto found = parent.find(name);
    if (found == parent.end() || !found->is_array())
        return result;
    for (const auto& value : *found)
        result.push_back(attachment(value));
    return result;
}

EffectCullMode cullMode(std::string_view value) {
    const auto key = compactKey(value);
    if (key == "none")
        return EffectCullMode::none;
    if (key == "front")
        return EffectCullMode::front;
    if (key == "back")
        return EffectCullMode::back;
    throw std::runtime_error("unsupported YRZFX cull mode: " + std::string(value));
}

EffectDepthFunc depthFunc(std::string_view value) {
    const auto key = compactKey(value);
    if (key == "never")
        return EffectDepthFunc::never;
    if (key == "less")
        return EffectDepthFunc::less;
    if (key == "equal")
        return EffectDepthFunc::equal;
    if (key == "lessequal")
        return EffectDepthFunc::lessEqual;
    if (key == "greater")
        return EffectDepthFunc::greater;
    if (key == "notequal")
        return EffectDepthFunc::notEqual;
    if (key == "greaterequal")
        return EffectDepthFunc::greaterEqual;
    if (key == "always")
        return EffectDepthFunc::always;
    throw std::runtime_error("unsupported YRZFX depth function: " + std::string(value));
}

FxCompareOp compareOp(std::string_view value) {
    switch (depthFunc(value)) {
    case EffectDepthFunc::never:
        return FxCompareOp::never;
    case EffectDepthFunc::less:
        return FxCompareOp::less;
    case EffectDepthFunc::equal:
        return FxCompareOp::equal;
    case EffectDepthFunc::lessEqual:
        return FxCompareOp::lessEqual;
    case EffectDepthFunc::greater:
        return FxCompareOp::greater;
    case EffectDepthFunc::notEqual:
        return FxCompareOp::notEqual;
    case EffectDepthFunc::greaterEqual:
        return FxCompareOp::greaterEqual;
    case EffectDepthFunc::always:
        return FxCompareOp::always;
    }
    return FxCompareOp::always;
}

FxFilter samplerFilter(std::string_view value) {
    const auto key = compactKey(value);
    if (key == "point" || key == "nearest")
        return FxFilter::point;
    if (key == "anisotropic" || key == "aniso")
        return FxFilter::anisotropic;
    return FxFilter::linear;
}

FxAddressMode addressMode(std::string_view value) {
    const auto key = compactKey(value);
    if (key == "mirror" || key == "mirroredrepeat")
        return FxAddressMode::mirror;
    if (key == "clamp" || key == "clamptoedge")
        return FxAddressMode::clamp;
    if (key == "border" || key == "clamptoborder")
        return FxAddressMode::border;
    return FxAddressMode::wrap;
}

FxStencilOp stencilOp(std::string_view value) {
    const auto key = compactKey(value);
    if (key == "zero")
        return FxStencilOp::zero;
    if (key == "replace")
        return FxStencilOp::replace;
    if (key == "incrsat" || key == "incrementclamp")
        return FxStencilOp::incrementClamp;
    if (key == "decrsat" || key == "decrementclamp")
        return FxStencilOp::decrementClamp;
    if (key == "invert")
        return FxStencilOp::invert;
    if (key == "incr" || key == "incrementwrap")
        return FxStencilOp::incrementWrap;
    if (key == "decr" || key == "decrementwrap")
        return FxStencilOp::decrementWrap;
    return FxStencilOp::keep;
}

FxLogicOp logicOp(std::string_view value) {
    const auto key = compactKey(value);
    constexpr std::pair<std::string_view, FxLogicOp> values[] = {
        {"clear", FxLogicOp::clear},
        {"and", FxLogicOp::andOp},
        {"andreverse", FxLogicOp::andReverse},
        {"copy", FxLogicOp::copy},
        {"andinverted", FxLogicOp::andInverted},
        {"noop", FxLogicOp::noOp},
        {"xor", FxLogicOp::xorOp},
        {"or", FxLogicOp::orOp},
        {"nor", FxLogicOp::nor},
        {"equivalence", FxLogicOp::equivalence},
        {"invert", FxLogicOp::invert},
        {"orreverse", FxLogicOp::orReverse},
        {"copyinverted", FxLogicOp::copyInverted},
        {"orinverted", FxLogicOp::orInverted},
        {"nand", FxLogicOp::nand},
        {"set", FxLogicOp::set},
    };
    for (const auto& [name, valueEnum] : values)
        if (key == name)
            return valueEnum;
    throw std::runtime_error("unsupported YRZFX logic operation: " + std::string(value));
}

FxBorderColor borderColor(std::string_view value) {
    const auto key = compactKey(value);
    if (key == "opaquewhite")
        return FxBorderColor::opaqueWhite;
    if (key == "opaqueblack")
        return FxBorderColor::opaqueBlack;
    return FxBorderColor::transparentBlack;
}

EffectVertexInputRate vertexInputRate(std::string_view value) {
    return compactKey(value).find("instance") != std::string::npos ? EffectVertexInputRate::instance
                                                                   : EffectVertexInputRate::vertex;
}

EffectVertexFormat vertexFormat(std::string_view value) {
    const auto key = compactKey(value);
    if (key == "r32float" || key == "float")
        return EffectVertexFormat::r32Float;
    if (key == "r32g32float" || key == "float2")
        return EffectVertexFormat::r32g32Float;
    if (key == "r32g32b32a32float" || key == "float4")
        return EffectVertexFormat::r32g32b32a32Float;
    return EffectVertexFormat::r32g32b32Float;
}

std::uint8_t colorWriteMask(const nlohmann::json& value) {
    if (value.is_number_unsigned())
        return static_cast<std::uint8_t>(value.get<std::uint32_t>() & 0x0FU);
    if (value.is_number_integer())
        return static_cast<std::uint8_t>(std::max(0, value.get<int>()) & 0x0F);
    if (!value.is_string())
        return 0x0FU;
    const auto key = compactKey(value.get<std::string>());
    if (key == "none" || key == "zero")
        return 0;
    std::uint8_t mask = 0;
    if (key.find('r') != std::string::npos)
        mask |= 0x1U;
    if (key.find('g') != std::string::npos)
        mask |= 0x2U;
    if (key.find('b') != std::string::npos)
        mask |= 0x4U;
    if (key.find('a') != std::string::npos)
        mask |= 0x8U;
    return mask == 0 ? 0x0FU : mask;
}

bool knownBlendFactor(std::string_view value) {
    const auto key = compactKey(value);
    return key == "zero" || key == "one" || key == "srccolor" || key == "invsrccolor" || key == "srcalpha" ||
           key == "invsrcalpha" || key == "destalpha" || key == "invdestalpha" || key == "destcolor" ||
           key == "invdestcolor" || key == "srcalphasaturate";
}

bool knownBlendOp(std::string_view value) {
    const auto key = compactKey(value);
    return key == "add" || key == "subtract" || key == "revsubtract" || key == "min" || key == "max";
}

std::string blendString(const nlohmann::json& parent, std::initializer_list<std::string_view> names) {
    for (const auto name : names) {
        const auto found = parent.find(name);
        if (found == parent.end())
            continue;
        if (!found->is_string())
            throw std::runtime_error("YRZFX blend state is not a string: " + std::string(name));
        return found->get<std::string>();
    }
    return {};
}

void validateBlendValue(std::string_view value, bool factor, std::string_view field) {
    if (value.empty())
        return;
    if ((factor && !knownBlendFactor(value)) || (!factor && !knownBlendOp(value)))
        throw std::runtime_error("unsupported YRZFX " + std::string(factor ? "blend factor" : "blend operation") +
                                 " for " + std::string(field) + ": " + std::string(value));
}

EffectBlendAttachmentState blendAttachment(const nlohmann::json& value) {
    if (!value.is_object())
        throw std::runtime_error("YRZFX blend render target must be an object");
    EffectBlendAttachmentState result;
    result.enabled = value.value("blendEnable", false);
    result.srcColor = blendString(value, {"srcBlend", "srcColor"});
    result.dstColor = blendString(value, {"destBlend", "dstBlend", "dstColor"});
    result.colorOp = blendString(value, {"blendOp", "colorOp"});
    result.srcAlpha = blendString(value, {"srcBlendAlpha", "srcAlpha"});
    result.dstAlpha = blendString(value, {"destBlendAlpha", "dstAlpha"});
    result.alphaOp = blendString(value, {"blendOpAlpha", "alphaOp"});
    if (const auto mask = value.find("colorWriteMask"); mask != value.end())
        result.colorWriteMask = colorWriteMask(*mask);
    validateBlendValue(result.srcColor, true, "srcBlend");
    validateBlendValue(result.dstColor, true, "destBlend");
    validateBlendValue(result.colorOp, false, "blendOp");
    validateBlendValue(result.srcAlpha, true, "srcBlendAlpha");
    validateBlendValue(result.dstAlpha, true, "destBlendAlpha");
    validateBlendValue(result.alphaOp, false, "blendOpAlpha");
    return result;
}

std::vector<EffectBlendAttachmentState> blendStates(const nlohmann::json& parent) {
    const auto found = parent.find("blendDesc");
    if (found == parent.end())
        return {};
    if (!found->is_object())
        throw std::runtime_error("YRZFX blendDesc must be an object");
    std::vector<EffectBlendAttachmentState> result;
    constexpr std::string_view prefix = "renderTarget";
    for (const auto& [name, value] : found->items()) {
        if (name == "independentBlendEnable" || name == "alphaToCoverageEnable")
            continue;
        if (!name.starts_with(prefix))
            throw std::runtime_error("unsupported YRZFX blendDesc field: " + name);
        const auto suffix = std::string_view(name).substr(prefix.size());
        if (suffix.empty())
            throw std::runtime_error("YRZFX blend render target index is missing");
        std::size_t index = 0;
        const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), index);
        if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size())
            throw std::runtime_error("invalid YRZFX blend render target index: " + name);
        if (index >= result.size())
            result.resize(index + 1U);
        result[index] = blendAttachment(value);
    }
    return result;
}

EffectGraphicsState graphicsState(const nlohmann::json& pass) {
    EffectGraphicsState result;
    if (const auto rasterizer = pass.find("rasterizerDesc"); rasterizer != pass.end()) {
        if (!rasterizer->is_object())
            throw std::runtime_error("YRZFX rasterizerDesc must be an object");
        const auto cull = rasterizer->find("cullMode");
        if (cull != rasterizer->end())
            result.rasterizer.cullMode = cullMode(cull->get<std::string>());
        if (const auto front = rasterizer->find("frontFace"); front != rasterizer->end()) {
            const auto key = compactKey(front->get<std::string>());
            if (key == "cw" || key == "clockwise")
                result.rasterizer.frontFace = EffectFrontFace::clockwise;
            else if (key == "ccw" || key == "counterclockwise")
                result.rasterizer.frontFace = EffectFrontFace::counterClockwise;
            else
                throw std::runtime_error("unsupported YRZFX front-face orientation: " + front->get<std::string>());
        }
    }
    if (const auto depth = pass.find("depthStencilDesc"); depth != pass.end()) {
        if (!depth->is_object())
            throw std::runtime_error("YRZFX depthStencilDesc must be an object");
        if (const auto enabled = depth->find("depthEnable"); enabled != depth->end())
            result.depthStencil.depthEnable = enabled->get<bool>();
        if (const auto mask = depth->find("depthWriteMask"); mask != depth->end()) {
            if (mask->is_boolean())
                result.depthStencil.depthWrite = mask->get<bool>();
            else {
                const auto key = compactKey(mask->get<std::string>());
                if (key == "all")
                    result.depthStencil.depthWrite = true;
                else if (key == "zero" || key == "none")
                    result.depthStencil.depthWrite = false;
                else
                    throw std::runtime_error("unsupported YRZFX depth write mask: " + mask->get<std::string>());
            }
        }
        if (const auto function = depth->find("depthFunc"); function != depth->end())
            result.depthStencil.depthFunc = depthFunc(function->get<std::string>());
        if (const auto enabled = depth->find("stencilEnable"); enabled != depth->end())
            result.depthStencil.stencilEnable = enabled->get<bool>();
        if (const auto mask = depth->find("stencilReadMask"); mask != depth->end())
            result.depthStencil.stencilReadMask = mask->get<std::uint32_t>();
        if (const auto mask = depth->find("stencilWriteMask"); mask != depth->end())
            result.depthStencil.stencilWriteMask = mask->get<std::uint32_t>();
        const auto parseFace = [&](std::string_view name, EffectDepthStencilState::StencilFace& face) {
            const auto found = depth->find(name);
            if (found == depth->end() || !found->is_object())
                return;
            if (const auto field = found->find("stencilFailOp"); field != found->end())
                face.fail = stencilOp(field->get<std::string>());
            if (const auto field = found->find("stencilPassOp"); field != found->end())
                face.pass = stencilOp(field->get<std::string>());
            if (const auto field = found->find("stencilDepthFailOp"); field != found->end())
                face.depthFail = stencilOp(field->get<std::string>());
            if (const auto field = found->find("stencilFunc"); field != found->end())
                face.compare = compareOp(field->get<std::string>());
        };
        parseFace("frontFace", result.depthStencil.front);
        parseFace("backFace", result.depthStencil.back);
    }
    if (const auto target = pass.find("rasterModelTarget"); target != pass.end())
        result.modelTarget = fx::resolveRasterModelTarget(target->get<std::string>());
    result.blend = blendStates(pass);
    result.independentBlend = pass.value("blendDesc", nlohmann::json::object()).value("independentBlendEnable", false);
    result.alphaToCoverage = pass.value("blendDesc", nlohmann::json::object()).value("alphaToCoverageEnable", false);
    if (const auto blend = pass.find("blendDesc"); blend != pass.end() && blend->is_object()) {
        if (const auto enabled = blend->find("logicOpEnable"); enabled != blend->end())
            result.logicOpEnable = enabled->get<bool>();
        if (const auto op = blend->find("logicOp"); op != blend->end() && op->is_string())
            result.logicOp = logicOp(op->get<std::string>());
    }
    return result;
}

EffectVertexLayout vertexLayout(const nlohmann::json& pass) {
    EffectVertexLayout result;
    const auto found = pass.find("layout");
    if (found == pass.end() || !found->is_array())
        return result;
    std::uint32_t location = 0;
    for (const auto& value : *found) {
        if (!value.is_object())
            continue;
        const auto slot = value.value("inputSlot", 0U);
        const auto rate = vertexInputRate(value.value("inputSlotClass", "PER_VERTEX_DATA"));
        const auto binding =
            std::ranges::find_if(result.bindings, [slot](const auto& item) { return item.binding == slot; });
        if (binding == result.bindings.end())
            result.bindings.push_back({slot, 0U, rate});
        const auto formatName = value.value("format", "UNKNOWN");
        const auto semantic = value.value("semanticName", "");
        const auto semanticIndex = value.value("semanticIndex", 0U);
        result.attributes.push_back(
            {.location = location++,
             .binding = slot,
             .format = vertexFormat(formatName),
             .offset = value.value("alignedByteOffset", std::numeric_limits<std::uint32_t>::max()),
             .semanticName = semantic,
             .semanticIndex = semanticIndex,
             .formatName = formatName});
    }
    return result;
}

EffectSlider slider(const nlohmann::json& value) {
    EffectSlider result;
    if (!value.is_object())
        return result;
    result.minimum = value.value("min", value.value("minimum", 0.0F));
    result.maximum = value.value("max", value.value("maximum", 1.0F));
    result.step = value.value("step", 0.0F);
    result.defaultValue = value.value("default", value.value("defaultValue", 0.0F));
    result.logarithmic = value.value("log", value.value("logarithmic", false));
    result.integer = value.value("int", false);
    return result;
}

EffectSize effectSize(const nlohmann::json& value) {
    EffectSize result;
    if (!value.is_object())
        return result;
    result.base = value.value("base", "");
    result.absolute = value.value("absolute", false);
    result.width = value.value("width", 1U);
    result.height = value.value("height", 1U);
    result.depth = value.value("depth", 1U);
    result.dimension = value.value("dimension", 0U);
    if (const auto ratio = value.find("ratio"); ratio != value.end() && ratio->is_object()) {
        result.widthRatio = ratio->value("x", 1.0F);
        result.heightRatio = ratio->value("y", 1.0F);
        result.depthRatio = ratio->value("z", 1.0F);
    }
    result.convX = value.value("convX", "x");
    result.convY = value.value("convY", "y");
    result.convZ = value.value("convZ", "z");
    result.rounding = value.value("rounding", "trunc");
    return result;
}

std::vector<EffectTexture> textures(const nlohmann::json& parent, std::string_view name) {
    std::vector<EffectTexture> result;
    const auto values = parent.find(name);
    if (values == parent.end() || !values->is_array())
        return result;
    for (const auto& value : *values) {
        if (!value.is_object())
            continue;
        EffectTexture texture;
        texture.name = value.value("name", "");
        texture.format = value.value("format", "");
        texture.view = value.value("view", "");
        texture.filename = value.value("filename", "");
        texture.shared = value.value("shared", "");
        texture.mipmap = value.value("mipmap", false);
        texture.conditions = strings(value, "conditions");
        if (const auto size = value.find("size"); size != value.end()) {
            texture.size = effectSize(*size);
            texture.widthRatio = texture.size.widthRatio;
            texture.heightRatio = texture.size.heightRatio;
        }
        result.push_back(std::move(texture));
    }
    return result;
}

EffectSampler sampler(const nlohmann::json& value) {
    EffectSampler result;
    if (!value.is_object())
        return result;
    result.name = value.value("name", "");
    result.filter = value.value("filter", "");
    result.addressU = value.value("addressU", "WRAP");
    result.addressV = value.value("addressV", "WRAP");
    result.addressW = value.value("addressW", result.addressV);
    result.filterKind = samplerFilter(result.filter);
    result.addressModeU = addressMode(result.addressU);
    result.addressModeV = addressMode(result.addressV);
    result.addressModeW = addressMode(result.addressW);
    result.mipLodBias = value.value("mipLodBias", 0.0F);
    result.maxAnisotropy =
        std::max(1U, value.value("maxAnisotropy", result.filterKind == FxFilter::anisotropic ? 16U : 1U));
    if (const auto compare = value.find("comparisonFunc"); compare != value.end() && compare->is_string())
        result.comparisonFunc = compareOp(compare->get<std::string>());
    if (const auto border = value.find("borderColor"); border != value.end() && border->is_string())
        result.borderColor = borderColor(border->get<std::string>());
    result.minLod = value.value("minLod", 0.0F);
    result.maxLod = value.value("maxLod", std::numeric_limits<float>::max());
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
    return loadEffectGraphFromText(path, source);
}

EffectGraph loadEffectGraphFromText(const std::filesystem::path& path, std::string_view source) {
    constexpr std::string_view jsonMarker = "[YRZFX]";
    constexpr std::string_view hlslMarker = "[HLSL]";
    const auto jsonStart = source.find(jsonMarker);
    const auto hlslStart = source.find(hlslMarker, jsonStart == std::string::npos ? 0 : jsonStart + jsonMarker.size());
    if (jsonStart == std::string::npos || hlslStart == std::string::npos || hlslStart <= jsonStart) {
        throw std::runtime_error("effect does not contain YRZFX/HLSL sections");
    }
    EffectGraph graph;
    graph.sourcePath = path;
    // Upstream keeps ABI includes and type declarations before [YRZFX]. Keep
    // that predecessor separate so generated declarations can be inserted
    // after the includes and before the executable shader body.
    graph.hlslPrefix = source.substr(0, jsonStart);
    graph.rawYrzfx =
        std::string(source.substr(jsonStart + jsonMarker.size(), hlslStart - jsonStart - jsonMarker.size()));
    graph.hlsl = source.substr(hlslStart + hlslMarker.size());
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
    graph.memos = strings(fx, "memos");
    graph.globalVarSizeSpecified = fx.contains("globalVarSize");
    graph.globalVarSize = fx.value("globalVarSize", graph.globalVarSize);
    if (const auto global = fx.find("globalVariables"); global != fx.end() && global->is_object()) {
        graph.globalVarSizeSpecified = graph.globalVarSizeSpecified || global->contains("size");
        graph.globalVarSize = global->value("size", graph.globalVarSize);
    }
    graph.textures = textures(fx, "textures");
    graph.textures3D = textures(fx, "textures3D");
    if (const auto values = fx.find("buffers"); values != fx.end() && values->is_array()) {
        for (const auto& value : *values) {
            if (!value.is_object())
                continue;
            EffectBuffer buffer;
            buffer.name = value.value("name", "");
            buffer.type = value.value("type", "");
            buffer.format = value.value("format", "");
            buffer.view = value.value("view", "");
            buffer.shared = value.value("shared", "");
            buffer.filename = value.value("filename", "");
            buffer.elementSize = value.value("elemSize", value.value("elementSize", 0U));
            if (buffer.elementSize == 0)
                buffer.elementSize = hlslElementSize(buffer.type);
            buffer.conditions = strings(value, "conditions");
            if (const auto size = value.find("size"); size != value.end())
                buffer.size = effectSize(*size);
            graph.buffers.push_back(std::move(buffer));
        }
    }
    if (const auto cloning = fx.find("meshCloning"); cloning != fx.end() && cloning->is_object())
        graph.meshCloneCount = std::max(1U, cloning->value("count", 1U));
    if (const auto material = fx.find("matDescs"); material != fx.end() && material->is_object()) {
        EffectMaterialDescriptor descriptor;
        descriptor.name = material->value("name", "");
        descriptor.templatePath = material->value("template", "");
        descriptor.defaultFile = material->value("defaultFile", "");
        if (descriptor.name.empty() || descriptor.templatePath.empty())
            throw std::runtime_error("YRZFX matDescs requires name and template");
        graph.materialDescriptor = std::move(descriptor);
    }
    if (const auto values = fx.find("samplers"); values != fx.end() && values->is_array()) {
        for (const auto& value : *values)
            graph.samplers.push_back(sampler(value));
    }
    if (const auto values = fx.find("controllers"); values != fx.end() && values->is_array()) {
        for (const auto& value : *values)
            if (value.is_object()) {
                EffectController controller;
                controller.name = value.value("name", "");
                controller.controllerName = value.value("controllerName", "");
                controller.item = value.value("item", "");
                controller.type = value.value("type", "");
                controller.description = value.value("description", "");
                controller.descriptions = strings(value, "desc");
                if (controller.description.empty() && !controller.descriptions.empty())
                    controller.description = controller.descriptions.front();
                if (const auto metadata = value.find("slider"); metadata != value.end())
                    controller.slider = slider(*metadata);
                else if (value.contains("min") || value.contains("max"))
                    controller.slider = slider(value);
                graph.controllers.push_back(std::move(controller));
            }
    }
    if (const auto values = fx.find("passes"); values != fx.end() && values->is_array()) {
        for (const auto& value : *values) {
            EffectPass pass;
            pass.name = value.value("name", "");
            const auto typeName = value.value("type", "");
            pass.type = passType(typeName);
            const auto typeKey = compactKey(typeName);
            if (typeKey == "copy")
                pass.functionalKind = EffectFunctionalPassKind::copy;
            else if (typeKey == "clearrtv")
                pass.functionalKind = EffectFunctionalPassKind::clearRtv;
            else if (typeKey == "clearuav" || typeKey == "clear")
                pass.functionalKind =
                    typeKey == "clearrtv" ? EffectFunctionalPassKind::clearRtv : EffectFunctionalPassKind::clearUav;
            else if (typeKey == "mipmapgen" || typeKey == "mipmap")
                pass.functionalKind = EffectFunctionalPassKind::mipmapGen;
            pass.vertexShader = value.value("vertexShader", "");
            pass.pixelShader = value.value("pixelShader", "");
            pass.computeShader = value.value("computeShader", "");
            pass.rayGenerationShader = value.value("raygenShader", "");
            pass.missShaders = strings(value, "missShader");
            pass.callableShaders = strings(value, "callableShader");
            if (pass.callableShaders.empty())
                pass.callableShaders = strings(value, "callableShaders");
            if (const auto groups = value.find("hitGroup"); groups != value.end() && groups->is_array()) {
                for (const auto& group : *groups)
                    if (group.is_object())
                        pass.hitGroups.push_back({group.value("type", ""), group.value("closestHit", ""),
                                                  group.value("anyHit", ""), group.value("intersection", "")});
            }
            pass.macros = strings(value, "macros");
            if (const auto threads = value.find("numthreads"); threads != value.end() && threads->is_object()) {
                pass.numThreads = {threads->value("x", 0U), threads->value("y", 0U), threads->value("z", 0U)};
            }
            pass.conditions = strings(value, "conditions");
            pass.inputs = attachments(value, "inputs");
            if (pass.inputs.empty())
                pass.inputs = attachments(value, "readResources");
            pass.renderTargets = attachments(value, "RTV");
            pass.unorderedAccess = attachments(value, "UAV");
            if (pass.type == EffectPassType::copy) {
                const auto appendStringAttachment = [&](std::vector<EffectAttachment>& target,
                                                        std::initializer_list<std::string_view> names) {
                    if (!target.empty())
                        return;
                    for (const auto name : names) {
                        const auto found = value.find(name);
                        if (found != value.end() && found->is_string()) {
                            target.push_back(attachment(*found));
                            return;
                        }
                    }
                };
                appendStringAttachment(pass.inputs, {"src", "source", "copySrc"});
                appendStringAttachment(pass.renderTargets, {"dest", "destination", "copyDest"});
            }
            if (pass.type == EffectPassType::oidn) {
                pass.oidnInput = value.value("input", value.value("beauty", ""));
                pass.oidnAlbedo = value.value("albedo", "");
                pass.oidnNormal = value.value("normal", "");
                pass.oidnOutput = value.value("output", "");
            }
            if (const auto target = value.find("target"); target != value.end() && target->is_string()) {
                if (pass.unorderedAccess.empty() &&
                    (pass.type == EffectPassType::clear || pass.type == EffectPassType::mipmap))
                    pass.unorderedAccess.push_back(attachment(*target));
            }
            if (const auto depth = value.find("DSV"); depth != value.end()) {
                if (depth->is_string())
                    pass.depth.name = depth->get<std::string>();
                else if (depth->is_object())
                    pass.depth = attachment(*depth);
            }
            pass.graphics = graphicsState(value);
            pass.rasterizer = pass.graphics.rasterizer;
            pass.depthStencil = pass.graphics.depthStencil;
            pass.blend.targets.fill({});
            pass.blend.alphaToCoverage = pass.graphics.alphaToCoverage;
            pass.blend.independentBlend = pass.graphics.independentBlend;
            pass.blend.logicOpEnable = pass.graphics.logicOpEnable;
            pass.blend.logicOp = pass.graphics.logicOp;
            pass.blend.targetCount = static_cast<std::uint32_t>(std::min<std::size_t>(pass.graphics.blend.size(), 8));
            for (std::size_t index = 0; index < pass.blend.targetCount; ++index)
                pass.blend.targets[index] = pass.graphics.blend[index];
            pass.rasterVertexBuffer = value.value("rasterVB", "");
            pass.rasterIndexBuffer = value.value("rasterIB", "");
            if (pass.graphics.modelTarget == fx::RasterModelTarget::buffer)
                pass.rasterSource =
                    pass.rasterVertexBuffer.empty() ? EffectRasterSource::vertexBufferless : EffectRasterSource::buffer;
            if (const auto source = value.find("rasterSource"); source != value.end() && source->is_string()) {
                const auto sourceKey = compactKey(source->get<std::string>());
                if (sourceKey == "buffer" || sourceKey == "vertexbuffer")
                    pass.rasterSource = EffectRasterSource::buffer;
                else if (sourceKey == "vertexbufferless" || sourceKey == "none")
                    pass.rasterSource = EffectRasterSource::vertexBufferless;
            }
            pass.vertexLayout = vertexLayout(value);
            if (const auto size = value.find("outputSize"); size != value.end() && size->is_object()) {
                pass.outputSize = effectSize(*size);
                pass.outputWidthRatio = pass.outputSize.widthRatio;
                pass.outputHeightRatio = pass.outputSize.heightRatio;
                if (const auto ratio = size->find("ratio"); ratio != size->end() && ratio->is_object()) {
                    pass.outputWidthRatio = ratio->value("x", 1.0F);
                    pass.outputHeightRatio = ratio->value("y", 1.0F);
                }
            }
            pass.maxPayloadSize = value.value("maxPayloadSize", 0U);
            pass.maxAttributeSize = value.value("maxAttributeSize", 0U);
            pass.maxRecursionDepth = value.value("maxRecursionDepth", 1U);
            pass.functional.source = pass.inputs.size() == 1 ? pass.inputs.front().name : "";
            pass.functional.destination = pass.renderTargets.size() == 1 ? pass.renderTargets.front().name : "";
            pass.functional.target = value.value("target", "");
            if (pass.functional.target.empty()) {
                if (pass.functional.destination.empty() && pass.unorderedAccess.size() == 1)
                    pass.functional.target = pass.unorderedAccess.front().name;
                else if (!pass.functional.destination.empty())
                    pass.functional.target = pass.functional.destination;
            }
            pass.functional.kind = pass.functionalKind;
            if (const auto clear = value.find("value"); clear != value.end())
                pass.functional.clearValue = clearValue(*clear);
            if (pass.functional.kind == EffectFunctionalPassKind::clearRtv && pass.renderTargets.size() == 1)
                pass.functional.clearValue = pass.renderTargets.front().clearValue;
            if (pass.functional.kind == EffectFunctionalPassKind::clearUav && pass.unorderedAccess.size() == 1)
                pass.functional.clearValue = pass.unorderedAccess.front().clearValue;
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
            .callableShaders = pass.callableShaders,
            .conditions = pass.conditions,
            .outputWidthRatio = pass.outputWidthRatio,
            .outputHeightRatio = pass.outputHeightRatio,
            .maxPayloadSize = pass.maxPayloadSize,
            .maxAttributeSize = pass.maxAttributeSize,
            .maxRecursionDepth = pass.maxRecursionDepth,
            .numThreads = pass.numThreads,
            .outputSize = pass.outputSize,
            .functionalKind = pass.functionalKind,
            .functional = pass.functional,
        };
        if (pass.type == EffectPassType::oidn) {
            const auto hasResource = [&](std::string_view name, bool write) {
                return std::ranges::any_of(compiled.resources, [&](const EffectResourceBinding& resource) {
                    return resource.resource == name && resource.write == write;
                });
            };
            const auto addRead = [&](std::string_view name) {
                if (name.empty() || hasResource(name, false))
                    return;
                compiled.resources.push_back({std::string(name), false});
                if (writers.contains(std::string(name)))
                    compiled.barriers.push_back("read-after-write:" + std::string(name));
            };
            const auto addWrite = [&](std::string_view name) {
                if (name.empty() || hasResource(name, true))
                    return;
                compiled.resources.push_back({std::string(name), true});
                if (writers.contains(std::string(name)))
                    compiled.barriers.push_back("write-after-write:" + std::string(name));
                writers[std::string(name)] = true;
            };
            for (const auto& input : pass.inputs)
                addRead(input.name);
            addRead(pass.oidnInput);
            addRead(pass.oidnAlbedo);
            addRead(pass.oidnNormal);
            std::string output = pass.oidnOutput;
            if (output.empty() && pass.renderTargets.size() == 1)
                output = pass.renderTargets.front().name;
            if (output.empty() && pass.unorderedAccess.size() == 1)
                output = pass.unorderedAccess.front().name;
            addWrite(output);
        } else {
            for (const auto& input : pass.inputs) {
                if (input.name.empty())
                    continue;
                compiled.resources.push_back({input.name, false});
                if (writers.contains(input.name))
                    compiled.barriers.push_back("read-after-write:" + input.name);
            }
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
        }
        result.passes.push_back(std::move(compiled));
    }
    return result;
}

EffectExecutionStats EffectExecutor::execute(const CompiledEffect& effect, const PassCallback& callback) const {
    EffectExecutionStats stats;
    for (const auto& pass : effect.passes) {
        stats.barriers += pass.barriers.size();
        if (pass.type == EffectPassType::rasterizer || pass.type == EffectPassType::postprocess) {
            ++stats.rasterPasses;
        } else if (pass.type == EffectPassType::compute) {
            ++stats.computePasses;
        } else if (pass.type == EffectPassType::raytracing) {
            ++stats.rayTracingPasses;
        } else if (pass.type == EffectPassType::oidn) {
            ++stats.oidnPasses;
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
    case EffectPassType::copy:
        return "copy";
    case EffectPassType::clear:
        return "clear";
    case EffectPassType::mipmap:
        return "mipmap";
    case EffectPassType::oidn:
        return "oidn";
    case EffectPassType::unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace dayo::core
