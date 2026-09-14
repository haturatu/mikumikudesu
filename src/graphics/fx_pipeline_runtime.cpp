#include "graphics/fx_pipeline_runtime.hpp"

#include "fx/fx_shader_source.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <filesystem>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace dayo::graphics {
namespace {

void setError(std::string* error, std::string message) {
    if (error != nullptr)
        *error = std::move(message);
}

std::string stageName(fx::FxShaderStage stage) {
    switch (stage) {
    case fx::FxShaderStage::vertex:
        return "vertex";
    case fx::FxShaderStage::fragment:
        return "fragment";
    case fx::FxShaderStage::compute:
        return "compute";
    case fx::FxShaderStage::rayGeneration:
        return "rayGeneration";
    case fx::FxShaderStage::miss:
        return "miss";
    case fx::FxShaderStage::closestHit:
        return "closestHit";
    case fx::FxShaderStage::anyHit:
        return "anyHit";
    case fx::FxShaderStage::intersection:
        return "intersection";
    case fx::FxShaderStage::callable:
        return "callable";
    }
    return "unknown";
}

ShaderStageMask deviceStage(fx::FxShaderStage stage) noexcept {
    switch (stage) {
    case fx::FxShaderStage::vertex:
        return ShaderStageMask::vertex;
    case fx::FxShaderStage::fragment:
        return ShaderStageMask::fragment;
    case fx::FxShaderStage::compute:
        return ShaderStageMask::compute;
    case fx::FxShaderStage::rayGeneration:
        return ShaderStageMask::rayGeneration;
    case fx::FxShaderStage::miss:
        return ShaderStageMask::miss;
    case fx::FxShaderStage::closestHit:
        return ShaderStageMask::closestHit;
    case fx::FxShaderStage::anyHit:
        return ShaderStageMask::anyHit;
    case fx::FxShaderStage::intersection:
        return ShaderStageMask::intersection;
    case fx::FxShaderStage::callable:
        return ShaderStageMask::callable;
    }
    return ShaderStageMask::none;
}

std::string macroKey(std::span<const std::string> macros) {
    std::ostringstream output;
    for (const auto& macro : macros)
        output << macro.size() << ':' << macro << ';';
    return output.str();
}

std::string passMacro(std::string_view name) {
    std::string result = "YRZ_PASS_";
    result.reserve(result.size() + name.size());
    for (const auto character : name) {
        const auto value = static_cast<unsigned char>(character);
        result.push_back(std::isalnum(value) || character == '_' ? character : '_');
    }
    return result;
}

std::string includeDirectoryKey(std::span<const std::filesystem::path> directories) {
    std::ostringstream output;
    for (const auto& directory : directories)
        output << directory.lexically_normal().string().size() << ':' << directory.lexically_normal().string() << ';';
    return output.str();
}

std::string lowerAscii(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value)
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    return result;
}

std::optional<std::filesystem::path> resolveIncludeCase(const std::filesystem::path& directory,
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

std::string normalizeIncludeCase(std::string_view source, const std::filesystem::path& directory) {
    std::string result;
    result.reserve(source.size());
    std::error_code error;
    std::size_t lineStart = 0;
    while (lineStart < source.size()) {
        const auto lineEnd = source.find('\n', lineStart);
        const auto length = lineEnd == std::string_view::npos ? source.size() - lineStart : lineEnd - lineStart;
        const auto line = source.substr(lineStart, length);
        const auto includeStart = line.find("#include");
        const auto quoteStart = includeStart == std::string_view::npos ? std::string_view::npos
                                                                          : line.find('"', includeStart + 8);
        const auto quoteEnd = quoteStart == std::string_view::npos ? std::string_view::npos
                                                                     : line.find('"', quoteStart + 1);
        if (quoteStart != std::string_view::npos && quoteEnd != std::string_view::npos) {
            const auto include = line.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
            const auto resolved = resolveIncludeCase(directory, include);
            if (resolved.has_value()) {
                const auto absoluteDirectory = std::filesystem::absolute(directory, error).lexically_normal();
                if (!error) {
                    const auto relative = std::filesystem::relative(*resolved, absoluteDirectory, error);
                    if (!error && !relative.empty()) {
                        result.append(line.substr(0, quoteStart + 1));
                        result.append(relative.generic_string());
                        result.append(line.substr(quoteEnd));
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

std::string upper(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value)
        result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
    return result;
}

PixelFormat textureFormat(std::string_view value) {
    const auto name = upper(value);
    if (name.empty() || name == "R8G8B8A8_UNORM")
        return PixelFormat::rgba8Unorm;
    if (name == "R8G8B8A8_SRGB")
        return PixelFormat::rgba8Srgb;
    if (name == "R16G16B16A16_FLOAT")
        return PixelFormat::rgba16Float;
    if (name == "R32G32B32A32_FLOAT")
        return PixelFormat::rgba32Float;
    if (name == "R8_UNORM")
        return PixelFormat::r8Unorm;
    if (name == "R16_FLOAT")
        return PixelFormat::r16Float;
    if (name == "R16G16_FLOAT")
        return PixelFormat::r16g16Float;
    if (name == "R32_FLOAT")
        return PixelFormat::r32Float;
    if (name == "R32G32_FLOAT")
        return PixelFormat::r32g32Float;
    if (name == "D32_FLOAT")
        return PixelFormat::depth32Float;
    throw std::invalid_argument("FX graphics target format is unsupported: " + std::string(value));
}

PixelFormat graphicsTargetFormat(const fx::FxProgram& program, const fx::FxDispatch& dispatch) {
    for (const auto& resource : dispatch.resources) {
        if (!resource.write)
            continue;
        const auto texture = std::find_if(program.textures.begin(), program.textures.end(),
                                          [&resource](const auto& declaration) {
                                              return declaration.name == resource.name;
                                          });
        if (texture != program.textures.end())
            return textureFormat(texture->format);
    }
    return PixelFormat::rgba16Float;
}

} // namespace

FxPipelineRuntime::~FxPipelineRuntime() {
    reset();
}

handles::ShaderHandle FxPipelineRuntime::compileShader(Device& device, const fx::FxProgram& program,
                                                        const fx::FxDispatch& dispatch, std::string_view entryPoint,
                                                        fx::FxShaderStage stage, const fx::FxShaderCompiler& compiler,
                                                        Entry& entry, std::uint32_t resourceSet,
                                                        const fx::FxNativeShaderSourceOptions& sourceOptions) {
    if (entryPoint.empty())
        throw std::invalid_argument("FX shader entry point is empty for pass " + dispatch.name);
    if (program.hlsl.empty())
        throw std::invalid_argument("FX program has no HLSL source for pass " + dispatch.name);

    fx::FxShaderKey key;
    fx::FxShaderCompileRequest request;
    request.macros = dispatch.macros;
    request.macros.push_back(passMacro(dispatch.name));
    const auto generatedSource = fx::makeNativeFxShaderSource(program, dispatch, resourceSet, sourceOptions);
    key.sourceHash = program.sourcePath.string() + "@" + std::to_string(program.sourceVersion) + "@" +
                     std::to_string(resourceSet);
    key.hlslHash = std::to_string(std::hash<std::string>{}(generatedSource));
    key.entryPoint = std::string(entryPoint);
    key.stage = stageName(stage);
    key.dxcVersion = compiler.executable().string();
    key.spirvTarget = "vulkan1.3";
    key.compatProfile = "fx-native";
    key.macros = macroKey(request.macros);

    // The compiler writes generated HLSL to a temporary directory. Add the
    // effect's directory explicitly so relative includes retain the same
    // resolution they have in the source tree.
    if (!program.sourcePath.empty()) {
        const auto sourceDirectory = program.sourcePath.parent_path();
        request.includeDirectories.push_back(sourceDirectory.empty() ? std::filesystem::path{"."}
                                                                      : sourceDirectory);
    }
    key.includeDirectories = includeDirectoryKey(request.includeDirectories);

    const auto sourceDirectory = program.sourcePath.empty() ? std::filesystem::path{"."}
                                                            : program.sourcePath.parent_path();
    request.hlsl = normalizeIncludeCase(generatedSource, sourceDirectory.empty() ? std::filesystem::path{"."}
                                                                                   : sourceDirectory);
    request.sourcePath = program.sourcePath;
    request.entryPoint = std::string(entryPoint);
    request.stage = stage;
    const auto artifact = shaderCache_.compileOrGet(key, request, compiler);
    const auto shader = device.createShaderEx({
        .spirv = std::span<const std::uint32_t>(artifact.spirv.data(), artifact.spirv.size()),
        .entryPoint = std::string(entryPoint),
        .stage = deviceStage(stage),
    });
    entry.shaders.push_back(shader);
    return shader;
}

bool FxPipelineRuntime::build(Device& device, const fx::FxProgram& program, const fx::FxShaderCompiler& compiler,
                              const LayoutResolver& resolveLayout, std::string* error, std::uint32_t resourceSet,
                              const fx::FxNativeShaderSourceOptions& sourceOptions) {
    if (error != nullptr)
        error->clear();
    reset();
    if (!resolveLayout) {
        setError(error, "FX pipeline build requires a pipeline-layout resolver");
        return false;
    }
    device_ = &device;
    try {
        for (const auto& dispatch : program.passes) {
            if (dispatch.kind == fx::FxOpKind::copy || dispatch.kind == fx::FxOpKind::clear ||
                dispatch.kind == fx::FxOpKind::mipmap)
                continue;
            if (dispatch.name.empty())
                throw std::invalid_argument("FX pipeline pass has an empty name");
            if (entries_.contains(dispatch.name))
                throw std::invalid_argument("FX pipeline pass name is duplicated: " + dispatch.name);
            const auto layout = resolveLayout(dispatch);
            if (!layout.has_value() || !layout->valid())
                throw std::invalid_argument("FX pipeline layout is unavailable: " + dispatch.name);

            Entry entry;
            switch (dispatch.kind) {
            case fx::FxOpKind::raster: {
                const auto* raster = std::get_if<fx::FxRasterDispatch>(&dispatch.executable);
                if (raster == nullptr || raster->vertexShader.empty() || raster->pixelShader.empty())
                    throw std::invalid_argument("raster FX pass requires vertex and pixel shaders: " + dispatch.name);
                const auto vertex = compileShader(device, program, dispatch, raster->vertexShader,
                                                  fx::FxShaderStage::vertex, compiler, entry, resourceSet,
                                                  sourceOptions);
                const auto pixel = compileShader(device, program, dispatch, raster->pixelShader,
                                                 fx::FxShaderStage::fragment, compiler, entry, resourceSet,
                                                 sourceOptions);
                entry.pipeline = device.createGraphicsPipelineEx({.layout = *layout,
                                                                   .shaders = {vertex, pixel},
                                                                   .colorFormat = graphicsTargetFormat(program,
                                                                                                       dispatch)});
                break;
            }
            case fx::FxOpKind::postprocess: {
                const auto* postprocess = std::get_if<fx::FxPostProcessDispatch>(&dispatch.executable);
                if (postprocess == nullptr || postprocess->pixelShader.empty())
                    throw std::invalid_argument("postprocess FX pass requires a pixel shader: " + dispatch.name);
                const auto fullscreenVertex = device.nativeFullscreenVertexShader();
                if (!fullscreenVertex.valid())
                    throw std::invalid_argument(
                        "postprocess FX pipeline needs a renderer-owned fullscreen vertex shader: " + dispatch.name);
                const auto pixel = compileShader(device, program, dispatch, postprocess->pixelShader,
                                                 fx::FxShaderStage::fragment, compiler, entry, resourceSet,
                                                 sourceOptions);
                entry.pipeline = device.createGraphicsPipelineEx({.layout = *layout,
                                                                    .shaders = {fullscreenVertex, pixel},
                                                                    .colorFormat = graphicsTargetFormat(program,
                                                                                                        dispatch)});
                break;
            }
            case fx::FxOpKind::compute: {
                const auto* compute = std::get_if<fx::FxComputeDispatch>(&dispatch.executable);
                if (compute == nullptr || compute->computeShader.empty())
                    throw std::invalid_argument("compute FX pass requires a compute shader: " + dispatch.name);
                const auto shader = compileShader(device, program, dispatch, compute->computeShader,
                                                  fx::FxShaderStage::compute, compiler, entry, resourceSet,
                                                  sourceOptions);
                entry.pipeline = device.createComputePipelineEx({.layout = *layout, .shaders = {shader}});
                break;
            }
            case fx::FxOpKind::raytracing: {
                const auto* ray = std::get_if<fx::FxRayTracingDispatch>(&dispatch.executable);
                if (ray == nullptr || ray->rayGenerationShader.empty())
                    throw std::invalid_argument("ray-tracing FX pass requires a raygen shader: " + dispatch.name);
                RayTracingPipelineDescEx descriptor;
                descriptor.layout = *layout;
                descriptor.maxPayloadSize = ray->maxPayloadSize;
                descriptor.maxAttributeSize = ray->maxAttributeSize;
                descriptor.maxRecursionDepth = ray->maxRecursionDepth;
                descriptor.rayGeneration.push_back(
                    compileShader(device, program, dispatch, ray->rayGenerationShader,
                                  fx::FxShaderStage::rayGeneration, compiler, entry, resourceSet, sourceOptions));
                for (const auto& shader : ray->missShaders)
                    descriptor.miss.push_back(
                        compileShader(device, program, dispatch, shader, fx::FxShaderStage::miss, compiler, entry,
                                      resourceSet, sourceOptions));
                for (const auto& group : ray->hitGroups) {
                    RayTracingHitGroupDesc hit;
                    hit.type = group.type == core::fx::FxRayTracingHitGroupType::procedural
                                   ? RayTracingHitGroupType::procedural
                                   : RayTracingHitGroupType::triangles;
                    if (!group.closestHit.empty())
                        hit.closestHit = compileShader(device, program, dispatch, group.closestHit,
                                                       fx::FxShaderStage::closestHit, compiler, entry, resourceSet,
                                                       sourceOptions);
                    if (!group.anyHit.empty())
                        hit.anyHit = compileShader(device, program, dispatch, group.anyHit,
                                                   fx::FxShaderStage::anyHit, compiler, entry, resourceSet,
                                                   sourceOptions);
                    if (!group.intersection.empty())
                        hit.intersection = compileShader(device, program, dispatch, group.intersection,
                                                         fx::FxShaderStage::intersection, compiler, entry, resourceSet,
                                                         sourceOptions);
                    descriptor.hitGroups.push_back(hit);
                }
                for (const auto& shader : ray->callableShaders)
                    descriptor.callable.push_back(compileShader(device, program, dispatch, shader,
                                                                fx::FxShaderStage::callable, compiler, entry,
                                                                resourceSet, sourceOptions));
                entry.pipeline = device.createRayTracingPipelineEx(descriptor);
                entry.sbt = device.createShaderBindingTable({
                    .pipeline = entry.pipeline,
                    .raygenCount = static_cast<std::uint32_t>(descriptor.rayGeneration.size()),
                    .missCount = static_cast<std::uint32_t>(descriptor.miss.size()),
                    .hitCount = static_cast<std::uint32_t>(descriptor.hitGroups.size()),
                    .callableCount = static_cast<std::uint32_t>(descriptor.callable.size()),
                });
                break;
            }
            case fx::FxOpKind::copy:
            case fx::FxOpKind::clear:
            case fx::FxOpKind::mipmap:
                break;
            }
            entries_.emplace(dispatch.name, std::move(entry));
        }
    } catch (const std::exception& exception) {
        setError(error, exception.what());
        reset();
        return false;
    } catch (...) {
        setError(error, "FX pipeline build failed");
        reset();
        return false;
    }
    return true;
}

void FxPipelineRuntime::destroyEntry(const Entry& entry) noexcept {
    if (device_ == nullptr)
        return;
    try {
        if (entry.sbt.valid())
            device_->destroyShaderBindingTable(entry.sbt);
        if (entry.pipeline.valid())
            device_->destroyPipelineEx(entry.pipeline);
        for (const auto shader : entry.shaders)
            if (shader.valid())
                device_->destroyShaderEx(shader);
    } catch (...) {
        // Runtime teardown must not hide the original pipeline error.
    }
}

void FxPipelineRuntime::reset() noexcept {
    if (device_ != nullptr) {
        try {
            device_->waitIdle();
        } catch (...) {
        }
        for (const auto& [name, entry] : entries_) {
            static_cast<void>(name);
            destroyEntry(entry);
        }
    }
    entries_.clear();
    device_ = nullptr;
}

std::optional<handles::PipelineHandle> FxPipelineRuntime::resolvePipeline(const fx::FxDispatch& dispatch) const {
    const auto it = entries_.find(dispatch.name);
    if (it == entries_.end() || !it->second.pipeline.valid())
        return std::nullopt;
    return it->second.pipeline;
}

std::optional<handles::ShaderBindingTableHandle>
FxPipelineRuntime::resolveShaderBindingTable(const fx::FxDispatch& dispatch) const {
    const auto it = entries_.find(dispatch.name);
    if (it == entries_.end() || !it->second.sbt.valid())
        return std::nullopt;
    return it->second.sbt;
}

} // namespace dayo::graphics
