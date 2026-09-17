#include "fx/fx_shader_source.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace dayo::fx {
namespace {

[[nodiscard]] std::string upper(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value)
        result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
    return result;
}

[[nodiscard]] std::string identifier(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) || character == '_')
            result.push_back(character);
        else
            result.push_back('_');
    }
    if (result.empty() || std::isdigit(static_cast<unsigned char>(result.front())))
        result.insert(result.begin(), '_');
    return result;
}

[[nodiscard]] std::string passMacro(std::string_view name) {
    return "YRZ_PASS_" + identifier(name);
}

[[nodiscard]] std::string elementType(std::string_view format) {
    const auto name = upper(format);
    if (name == "R16G16_FLOAT" || name == "R32G32_FLOAT")
        return "float2";
    if (name == "R8G8B8A8_UNORM" || name == "R8G8B8A8_SRGB" || name == "R16G16B16A16_FLOAT" ||
        name == "R32G32B32A32_FLOAT" || name.empty())
        return "float4";
    if (name == "R8_UNORM" || name == "R16_FLOAT" || name == "R32_FLOAT" || name == "D32_FLOAT")
        return "float";
    throw std::invalid_argument("FX shader source has an unsupported texture format: " + std::string(format));
}

[[nodiscard]] std::string resourceSetSuffix(std::uint32_t resourceSet) {
    return ", space" + std::to_string(resourceSet);
}

[[nodiscard]] char registerPrefix(FxNativeShaderRegister registerClass) noexcept {
    switch (registerClass) {
    case FxNativeShaderRegister::uav:
        return 'u';
    case FxNativeShaderRegister::sampled:
        return 't';
    case FxNativeShaderRegister::sampler:
        return 's';
    case FxNativeShaderRegister::uniform:
        return 'b';
    }
    return 't';
}

[[nodiscard]] std::string controllerName(std::string_view name, std::string& arraySuffix) {
    arraySuffix.clear();
    const auto bracket = name.find('[');
    if (bracket == std::string_view::npos)
        return identifier(name);
    if (name.empty() || name.back() != ']')
        throw std::invalid_argument("FX controller has an invalid array name: " + std::string(name));
    const auto countText = name.substr(bracket + 1, name.size() - bracket - 2);
    if (countText.empty())
        throw std::invalid_argument("FX controller has an empty array size: " + std::string(name));
    std::size_t parsed = 0;
    for (const auto character : countText) {
        if (!std::isdigit(static_cast<unsigned char>(character)))
            throw std::invalid_argument("FX controller has an invalid array size: " + std::string(name));
        const auto digit = static_cast<std::size_t>(character - '0');
        if (parsed > (std::numeric_limits<std::size_t>::max() - digit) / 10U)
            throw std::overflow_error("FX controller array size overflow: " + std::string(name));
        parsed = parsed * 10U + digit;
    }
    if (parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("FX controller array size is out of range: " + std::string(name));
    arraySuffix = "[" + std::to_string(parsed) + "]";
    return identifier(name.substr(0, bracket));
}

[[nodiscard]] bool isUav(std::string_view view) {
    const auto name = upper(view);
    return name.find("UAV") != std::string::npos || name.find("STORAGE") != std::string::npos;
}

void appendControllerBlock(std::ostringstream& output, const FxProgram& program) {
    if (program.controllers.empty())
        return;
    output << "cbuffer YRZFX_ControllerCB : register(b1) {\n";
    std::unordered_set<std::string> names;
    for (const auto& controller : program.controllers) {
        std::string arraySuffix;
        const auto name = controllerName(controller.name, arraySuffix);
        if (!names.insert(name).second)
            throw std::invalid_argument("FX controller name is duplicated: " + name);
        const auto type = controller.type.empty() ? std::string_view{"float"} : std::string_view{controller.type};
        if (type != "bool" && type != "int" && type != "uint" && type != "float" && type != "float2" &&
            type != "float3" && type != "float4" && type != "float4x4")
            throw std::invalid_argument("FX controller type is unsupported: " + std::string(type));
        output << "    " << type << ' ' << name << arraySuffix << ";\n";
    }
    output << "};\n";
}

void appendTextureDeclarations(std::ostringstream& output, const FxProgram& program, std::uint32_t resourceSet,
                               std::uint32_t& sampledBinding, std::uint32_t& uavBinding) {
    for (const auto& texture : program.textures) {
        const auto write = isUav(texture.view);
        const auto binding = write ? uavBinding++ : sampledBinding++;
        output << (write ? "RWTexture2D<" : "Texture2D<") << elementType(texture.format) << "> "
               << identifier(texture.name) << " : register(" << (write ? 'u' : 't') << binding
               << resourceSetSuffix(resourceSet) << ");\n";
    }
    for (const auto& texture : program.textures3D) {
        const auto write = isUav(texture.view);
        const auto binding = write ? uavBinding++ : sampledBinding++;
        output << (write ? "RWTexture3D<" : "Texture3D<") << elementType(texture.format) << "> "
               << identifier(texture.name) << " : register(" << (write ? 'u' : 't') << binding
               << resourceSetSuffix(resourceSet) << ");\n";
    }
}

void appendBufferDeclarations(std::ostringstream& output, const FxProgram& program, std::uint32_t resourceSet,
                              std::uint32_t& sampledBinding, std::uint32_t& uavBinding) {
    for (const auto& buffer : program.buffers) {
        const auto write = isUav(buffer.view);
        const auto binding = write ? uavBinding++ : sampledBinding++;
        const auto type = buffer.type.empty() ? std::string_view{"uint"} : std::string_view{buffer.type};
        output << (write ? "RWStructuredBuffer<" : "StructuredBuffer<") << type << "> " << identifier(buffer.name)
               << " : register(" << (write ? 'u' : 't') << binding << resourceSetSuffix(resourceSet) << ");\n";
    }
}

void appendSamplerDeclarations(std::ostringstream& output, const FxProgram& program, std::uint32_t resourceSet,
                               std::uint32_t& binding) {
    for (const auto& sampler : program.samplers) {
        output << "SamplerState " << identifier(sampler.name) << " : register(s" << binding
               << resourceSetSuffix(resourceSet) << ");\n";
        ++binding;
    }
}

void appendSharedDeclarations(std::ostringstream& output, const FxNativeShaderSourceOptions& options) {
    if (!options.preamble.empty()) {
        output << options.preamble;
        if (options.preamble.back() != '\n')
            output << '\n';
    }
    std::unordered_set<std::string> names;
    for (const auto& resource : options.resources) {
        if (resource.declaration.empty())
            throw std::invalid_argument("native FX shared resource declaration is empty");
        if (!names.insert(resource.declaration).second)
            throw std::invalid_argument("native FX shared resource declaration is duplicated: " + resource.declaration);
        output << resource.declaration << " : register(" << registerPrefix(resource.registerClass)
               << resource.registerIndex << resourceSetSuffix(resource.descriptorSet) << ");\n";
    }
}

} // namespace

std::string makeNativeFxShaderSource(const FxProgram& program, const FxDispatch& dispatch, std::uint32_t resourceSet,
                                     const FxNativeShaderSourceOptions& options) {
    std::ostringstream output;
    output << "// generated native FX declarations\n";
    appendSharedDeclarations(output, options);
    appendControllerBlock(output, program);
    output << "#ifdef " << passMacro(dispatch.name) << "\n";
    std::uint32_t sampledBinding = 0;
    std::uint32_t uavBinding = 0;
    std::uint32_t samplerBinding = 0;
    appendTextureDeclarations(output, program, resourceSet, sampledBinding, uavBinding);
    appendBufferDeclarations(output, program, resourceSet, sampledBinding, uavBinding);
    appendSamplerDeclarations(output, program, resourceSet, samplerBinding);
    output << "#endif\n";
    output << program.hlsl;
    return output.str();
}

} // namespace dayo::fx
