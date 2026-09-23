#include "fx/fx_shader_source.hpp"

#include "core/fx/fx_material.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

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

[[nodiscard]] core::fx::MaterialTemplateSchema parseMaterialTemplate(const FxProgram& program) {
    if (!program.materialDescriptor.has_value())
        return {};
    if (program.materialSchema.has_value())
        return *program.materialSchema;
    const auto& descriptor = *program.materialDescriptor;
    const auto path = descriptor.templatePath.is_absolute()
                          ? descriptor.templatePath
                          : program.sourcePath.parent_path() / descriptor.templatePath;
    auto schema = core::fx::loadMaterialTemplateSchema(path, descriptor.name);
    if (!descriptor.defaultFile.empty()) {
        const auto defaults = descriptor.defaultFile.is_absolute()
                                  ? descriptor.defaultFile
                                  : program.sourcePath.parent_path() / descriptor.defaultFile;
        core::fx::loadMaterialDefaultFile(schema, defaults);
    }
    return schema;
}

[[nodiscard]] bool dispatchWrites(const FxDispatch& dispatch, std::string_view name) {
    return std::ranges::any_of(dispatch.resources, [name](const FxDispatch::ResourceUse& resource) {
        return resource.write && resource.name == name && resource.role != FxResourceRole::colorAttachment &&
               resource.role != FxResourceRole::depthAttachment;
    });
}

[[nodiscard]] bool dispatchUsesAsColor(const FxDispatch& dispatch, std::string_view name) {
    return std::ranges::any_of(dispatch.resources, [name](const FxDispatch::ResourceUse& resource) {
        return resource.write && resource.name == name && resource.role == FxResourceRole::colorAttachment;
    });
}

[[nodiscard]] bool dispatchUsesAsDepth(const FxDispatch& dispatch, std::string_view name) {
    return std::ranges::any_of(dispatch.resources, [name](const FxDispatch::ResourceUse& resource) {
        return resource.write && resource.name == name && resource.role == FxResourceRole::depthAttachment;
    });
}

[[nodiscard]] bool containsIdentifier(std::string_view source, std::string_view wanted) {
    std::size_t offset = 0;
    while ((offset = source.find(wanted, offset)) != std::string_view::npos) {
        const auto before = offset == 0 ? '\0' : source[offset - 1];
        const auto after = offset + wanted.size() == source.size() ? '\0' : source[offset + wanted.size()];
        const auto isIdentifier = [](char value) {
            return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
        };
        if (!isIdentifier(before) && !isIdentifier(after))
            return true;
        offset += wanted.size();
    }
    return false;
}

void appendLegacyCompatibilityDeclarations(std::ostringstream& output, const FxProgram& program) {
    const auto hasLocalGBuffer =
        std::ranges::any_of(program.textures, [](const auto& texture) { return texture.name == "GBuffer"; });
    if (!hasLocalGBuffer &&
        (containsIdentifier(program.hlslPrefix, "GBuffer") || containsIdentifier(program.hlsl, "GBuffer")))
        output << "Texture2D<float4> GBuffer : register(t12, space0);\n";
}

void appendMaterialDeclarations(std::ostringstream& output, const FxProgram& program, const FxDispatch& dispatch,
                                std::uint32_t resourceSet, std::uint32_t& sampledBinding) {
    if (!program.materialDescriptor.has_value())
        return;
    const auto material = parseMaterialTemplate(program);
    const auto& name = program.materialDescriptor->name;
    output << "struct " << identifier(name) << "Value {\n";
    for (const auto& field : material.fields) {
        const auto type = field.type == core::fx::MaterialFieldType::floatingPoint ? "float" : "int";
        output << "    " << type;
        if (field.components > 1)
            output << field.components;
        output << ' ' << field.name << ";\n";
    }
    output << "};\n";
    output << "struct " << identifier(name) << "Texture {\n";
    for (const auto& field : material.textures)
        if (field.dimension == core::fx::MaterialTextureDimension::twoD)
            output << "    bool has" << field.name << "; Texture2D<float4> " << field.name << ";\n";
    output << "};\n";
    output << "struct " << identifier(name) << "Texture3D {\n";
    for (const auto& field : material.textures)
        if (field.dimension == core::fx::MaterialTextureDimension::threeD)
            output << "    bool has" << field.name << "; Texture3D<float4> " << field.name << ";\n";
    output << "};\n";

    const auto indexBinding = sampledBinding++;
    const auto textureIndexBinding = sampledBinding++;
    const auto texture3dIndexBinding = sampledBinding++;
    const auto valueBinding = sampledBinding++;
    const auto textureBinding = sampledBinding++;
    std::uint32_t textureSlotCount = 0;
    for (const auto& field : material.textures)
        textureSlotCount = std::max(textureSlotCount, field.index + 1U);
    output << "StructuredBuffer<uint> " << identifier(name) << "_idx : register(t" << indexBinding
           << resourceSetSuffix(resourceSet) << ");\n";
    output << "StructuredBuffer<uint> " << identifier(name) << "_tex : register(t" << textureIndexBinding
           << resourceSetSuffix(resourceSet) << ");\n";
    output << "StructuredBuffer<uint> " << identifier(name) << "_tex3D : register(t" << texture3dIndexBinding
           << resourceSetSuffix(resourceSet) << ");\n";
    output << "StructuredBuffer<" << identifier(name) << "Value> " << identifier(name) << "_value : register(t"
           << valueBinding << resourceSetSuffix(resourceSet) << ");\n";
    output << "Texture2D<float4> " << identifier(name) << "_texture[] : register(t" << textureBinding
           << resourceSetSuffix(resourceSet) << ");\n";
    output << "Texture3D<float4> " << identifier(name) << "_texture3D[] : register(t0"
           << resourceSetSuffix(resourceSet + 1U) << ");\n";
    output << identifier(name) << "Value Get" << identifier(name) << "Value(uint ID, uint subID) {\n"
           << "    uint imat = " << identifier(name) << "_idx[ID] + subID;\n"
           << "    return " << identifier(name) << "_value[imat];\n}\n";
    output << identifier(name) << "Texture Get" << identifier(name) << "Texture(uint ID, uint subID) {\n"
           << "    " << identifier(name) << "Texture result = (" << identifier(name) << "Texture)0;\n"
           << "    uint imat = " << identifier(name) << "_idx[ID] + subID;\n"
           << "    uint tidx = imat * " << textureSlotCount << ";\n";
    for (const auto& field : material.textures) {
        if (field.dimension == core::fx::MaterialTextureDimension::threeD)
            continue;
        output << "    result.has" << field.name << " = (" << identifier(name) << "_tex[tidx + " << field.index
               << "] != 0xffffffff);\n"
               << "    result." << field.name << " = " << identifier(name) << "_texture[NonUniformResourceIndex("
               << identifier(name) << "_tex[tidx + " << field.index << "])];\n";
    }
    output << "    return result;\n}\n";
    output << identifier(name) << "Texture3D Get" << identifier(name) << "Texture3D(uint ID, uint subID) {\n"
           << "    " << identifier(name) << "Texture3D result = (" << identifier(name) << "Texture3D)0;\n"
           << "    uint imat = " << identifier(name) << "_idx[ID] + subID;\n"
           << "    uint tidx = imat * " << textureSlotCount << ";\n";
    for (const auto& field : material.textures) {
        if (field.dimension == core::fx::MaterialTextureDimension::twoD)
            continue;
        output << "    result.has" << field.name << " = (" << identifier(name) << "_tex3D[tidx + " << field.index
               << "] != 0xffffffff);\n"
               << "    result." << field.name << " = " << identifier(name) << "_texture3D[NonUniformResourceIndex("
               << identifier(name) << "_tex3D[tidx + " << field.index << "])];\n";
    }
    output << "    return result;\n}\n";
    static_cast<void>(dispatch);
}

void appendControllerBlock(std::ostringstream& output, const FxProgram& program,
                           std::span<const core::EffectController> sharedControllers) {
    const auto controllers =
        sharedControllers.empty() ? std::span<const core::EffectController>(program.controllers) : sharedControllers;
    if (controllers.empty())
        return;
    output << "cbuffer YRZFX_ControllerCB : register(b1) {\n";
    std::unordered_set<std::string> names;
    for (const auto& controller : controllers) {
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

void appendTextureDeclarations(std::ostringstream& output, const FxProgram& program, const FxDispatch& dispatch,
                               std::uint32_t resourceSet, const FxPassBindingPlan& bindings) {
    for (const auto& texture : program.textures) {
        const auto write = dispatchWrites(dispatch, texture.name);
        const auto color = dispatchUsesAsColor(dispatch, texture.name);
        const auto depth = dispatchUsesAsDepth(dispatch, texture.name);
        if (color || depth) {
            output << "Texture2D<" << elementType(texture.format) << "> " << identifier(texture.name) << ";\n";
            continue;
        }
        const auto* planned = bindings.find(texture.name);
        if (planned == nullptr)
            throw std::logic_error("FX binding plan omitted texture: " + texture.name);
        const auto binding = planned->binding - fxDescriptorBindingBaseForUse(planned->descriptorClass, write);
        output << (write ? "RWTexture2D<" : "Texture2D<") << elementType(texture.format) << "> "
               << identifier(texture.name) << " : register(" << (write ? 'u' : 't') << binding
               << resourceSetSuffix(resourceSet) << ");\n";
    }
    for (const auto& texture : program.textures3D) {
        const auto write = dispatchWrites(dispatch, texture.name);
        const auto color = dispatchUsesAsColor(dispatch, texture.name);
        const auto depth = dispatchUsesAsDepth(dispatch, texture.name);
        if (color || depth) {
            output << "Texture3D<" << elementType(texture.format) << "> " << identifier(texture.name) << ";\n";
            continue;
        }
        const auto* planned = bindings.find(texture.name);
        if (planned == nullptr)
            throw std::logic_error("FX binding plan omitted 3D texture: " + texture.name);
        const auto binding = planned->binding - fxDescriptorBindingBaseForUse(planned->descriptorClass, write);
        output << (write ? "RWTexture3D<" : "Texture3D<") << elementType(texture.format) << "> "
               << identifier(texture.name) << " : register(" << (write ? 'u' : 't') << binding
               << resourceSetSuffix(resourceSet) << ");\n";
    }
}

void appendBufferDeclarations(std::ostringstream& output, const FxProgram& program, const FxDispatch& dispatch,
                              std::uint32_t resourceSet, const FxPassBindingPlan& bindings) {
    for (const auto& buffer : program.buffers) {
        const auto write = dispatchWrites(dispatch, buffer.name);
        const auto* planned = bindings.find(buffer.name);
        if (planned == nullptr)
            throw std::logic_error("FX binding plan omitted buffer: " + buffer.name);
        const auto binding = planned->binding - fxDescriptorBindingBaseForUse(planned->descriptorClass, write);
        const auto type = buffer.type.empty() ? std::string_view{"uint"} : std::string_view{buffer.type};
        output << (write ? "RWStructuredBuffer<" : "StructuredBuffer<") << type << "> " << identifier(buffer.name)
               << " : register(" << (write ? 'u' : 't') << binding << resourceSetSuffix(resourceSet) << ");\n";
    }
}

void appendSamplerDeclarations(std::ostringstream& output, const FxProgram& program, std::uint32_t resourceSet,
                               const FxPassBindingPlan& bindings) {
    for (const auto& sampler : program.samplers) {
        const auto* planned = bindings.find(sampler.name);
        if (planned == nullptr)
            throw std::logic_error("FX binding plan omitted sampler: " + sampler.name);
        const auto binding = planned->binding - fxDescriptorBindingBase(planned->descriptorClass);
        output << "SamplerState " << identifier(sampler.name) << " : register(s" << binding
               << resourceSetSuffix(resourceSet) << ");\n";
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

[[nodiscard]] std::string lowerAscii(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value)
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    return result;
}

[[nodiscard]] std::optional<std::filesystem::path> resolveIncludeCaseDirect(const std::filesystem::path& directory,
                                                                            std::string_view include) {
    const std::filesystem::path requested(include);
    if (requested.is_absolute())
        return std::nullopt;

    std::filesystem::path current = directory;
    std::error_code error;
    for (const auto& component : requested) {
        const auto name = component.string();
        if (name.empty() || name == ".")
            continue;
        if (name == "..") {
            current = current.parent_path();
            continue;
        }
        const auto exact = current / component;
        if (std::filesystem::is_directory(exact, error) || std::filesystem::is_regular_file(exact, error)) {
            current = exact;
            continue;
        }
        error.clear();
        if (!std::filesystem::is_directory(current, error))
            return std::nullopt;
        const auto wanted = lowerAscii(name);
        std::optional<std::filesystem::path> match;
        for (std::filesystem::directory_iterator iterator(current, error), end; !error && iterator != end;
             iterator.increment(error)) {
            if (lowerAscii(iterator->path().filename().string()) != wanted)
                continue;
            if (match.has_value())
                return std::nullopt;
            match = iterator->path();
        }
        if (!match.has_value())
            return std::nullopt;
        current = *match;
    }
    if (!std::filesystem::is_regular_file(current, error))
        return std::nullopt;
    return current;
}

[[nodiscard]] std::optional<std::filesystem::path> resolveIncludeCase(const std::filesystem::path& directory,
                                                                      std::string_view include) {
    if (const auto direct = resolveIncludeCaseDirect(directory, include); direct.has_value())
        return direct;

    // The upstream assets mix includes relative to the effect file with
    // includes written as if they were resolved from MikuMikuDayo/hlsl.
    // Generated HLSL is compiled from a temporary directory, so try the
    // canonical hlsl directory while retaining the original include spelling
    // in the generated source's relative path.
    const auto filename = std::filesystem::path(include).filename().string();
    if (filename.empty())
        return std::nullopt;
    std::filesystem::path current = directory;
    std::optional<std::filesystem::path> match;
    for (;;) {
        for (const auto& base : {current, current / "hlsl"}) {
            const auto candidate = resolveIncludeCaseDirect(base, filename);
            if (!candidate.has_value())
                continue;
            if (match.has_value() && *match != *candidate)
                return std::nullopt;
            match = candidate;
        }
        const auto parent = current.parent_path();
        if (parent == current || current.empty())
            break;
        current = parent;
    }
    return match;
}

} // namespace

std::string makeNativeFxShaderSource(const FxProgram& program, const FxDispatch& dispatch, std::uint32_t resourceSet,
                                     const FxNativeShaderSourceOptions& options) {
    std::ostringstream output;
    output << program.hlslPrefix;
    if (!program.hlslPrefix.empty() && program.hlslPrefix.back() != '\n')
        output << '\n';
    if (dispatch.kind == FxOpKind::compute) {
        auto threads = dispatch.numThreads;
        const bool unspecified = threads[0] == 0 && threads[1] == 0 && threads[2] == 0;
        if (unspecified) {
            auto dimension = dispatch.outputSize.dimension;
            if (dimension < 1 || dimension > 3)
                dimension = dispatch.outputSize.depth > 1                       ? 3U
                            : dispatch.outputSize.height > 1                    ? 2U
                            : dispatch.category == core::fx::FxCategory::deform ? 1U
                                                                                : 2U;
            if (dimension == 1)
                threads = {1024, 1, 1};
            else if (dimension == 2)
                threads = {16, 16, 1};
            else
                threads = {8, 8, 8};
        } else {
            for (auto& count : threads)
                count = std::max(count, 1U);
        }
        output << "#define YRZ_NUMTHREADS [numthreads(" << threads[0] << ',' << threads[1] << ',' << threads[2]
               << ")]\n";
    }
    output << "// generated native FX declarations\n";
    appendSharedDeclarations(output, options);
    appendControllerBlock(output, program, options.controllerDeclarations);
    appendLegacyCompatibilityDeclarations(output, program);
    output << "#ifdef " << passMacro(dispatch.name) << "\n";
    const auto bindings = planPassBindings(program, dispatch, resourceSet);
    std::uint32_t sampledBinding = 0;
    for (const auto& binding : bindings.bindings) {
        if (binding.descriptorClass == FxDescriptorClass::sampledImage ||
            (binding.descriptorClass == FxDescriptorClass::storageBuffer && !binding.writable)) {
            const auto registerIndex = binding.binding - fxDescriptorBindingBaseForUse(binding.descriptorClass, false);
            sampledBinding = std::max(sampledBinding, registerIndex + 1U);
        }
    }
    appendTextureDeclarations(output, program, dispatch, resourceSet, bindings);
    appendBufferDeclarations(output, program, dispatch, resourceSet, bindings);
    appendMaterialDeclarations(output, program, dispatch, resourceSet, sampledBinding);
    appendSamplerDeclarations(output, program, resourceSet, bindings);
    output << "#endif\n";
    output << program.generatedCode;
    if (!program.generatedCode.empty() && program.generatedCode.back() != '\n')
        output << '\n';
    output << program.hlsl;
    return output.str();
}

std::string normalizeFxShaderIncludes(std::string_view source, const std::filesystem::path& directory) {
    std::string result;
    result.reserve(source.size());
    std::error_code error;
    std::size_t lineStart = 0;
    while (lineStart < source.size()) {
        const auto lineEnd = source.find('\n', lineStart);
        const auto length = lineEnd == std::string_view::npos ? source.size() - lineStart : lineEnd - lineStart;
        const auto line = source.substr(lineStart, length);
        const auto includeStart = line.find("#include");
        const auto includeStartDelimiter = includeStart == std::string_view::npos
                                               ? std::string_view::npos
                                               : line.find_first_of("\"<", includeStart + 8);
        const auto closingDelimiter = includeStartDelimiter == std::string_view::npos ? '\0'
                                      : line[includeStartDelimiter] == '<'            ? '>'
                                                                                      : '"';
        const auto includeEndDelimiter = includeStartDelimiter == std::string_view::npos
                                             ? std::string_view::npos
                                             : line.find(closingDelimiter, includeStartDelimiter + 1);
        if (includeStartDelimiter != std::string_view::npos && includeEndDelimiter != std::string_view::npos) {
            const auto include =
                line.substr(includeStartDelimiter + 1, includeEndDelimiter - includeStartDelimiter - 1);
            const auto resolved = resolveIncludeCase(directory, include);
            if (resolved.has_value()) {
                const auto absoluteDirectory = std::filesystem::absolute(directory, error).lexically_normal();
                if (!error) {
                    const auto relative = std::filesystem::relative(*resolved, absoluteDirectory, error);
                    if (!error && !relative.empty()) {
                        result.append(line.substr(0, includeStartDelimiter + 1));
                        result.append(relative.generic_string());
                        result.append(line.substr(includeEndDelimiter));
                    } else {
                        result.append(line);
                    }
                } else {
                    result.append(line);
                    error.clear();
                }
            } else {
                result.append(line);
            }
        } else {
            result.append(line);
        }
        if (lineEnd != std::string_view::npos)
            result.push_back('\n');
        if (lineEnd == std::string_view::npos)
            break;
        lineStart = lineEnd + 1;
    }
    return result;
}

} // namespace dayo::fx
