#include "graphics/fx_pipeline_runtime.hpp"

#include <functional>
#include <sstream>
#include <span>
#include <stdexcept>
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

} // namespace

FxPipelineRuntime::~FxPipelineRuntime() {
    reset();
}

handles::ShaderHandle FxPipelineRuntime::compileShader(Device& device, const fx::FxProgram& program,
                                                        const fx::FxDispatch& dispatch, std::string_view entryPoint,
                                                        fx::FxShaderStage stage, const fx::FxShaderCompiler& compiler,
                                                        Entry& entry) {
    if (entryPoint.empty())
        throw std::invalid_argument("FX shader entry point is empty for pass " + dispatch.name);
    if (program.hlsl.empty())
        throw std::invalid_argument("FX program has no HLSL source for pass " + dispatch.name);

    fx::FxShaderKey key;
    key.sourceHash = program.sourcePath.string() + "@" + std::to_string(program.sourceVersion);
    key.hlslHash = std::to_string(std::hash<std::string>{}(program.hlsl));
    key.entryPoint = std::string(entryPoint);
    key.stage = stageName(stage);
    key.dxcVersion = compiler.executable().string();
    key.spirvTarget = "vulkan1.3";
    key.compatProfile = "fx-native";
    key.macros = macroKey(dispatch.macros);

    fx::FxShaderCompileRequest request;
    request.hlsl = program.hlsl;
    request.sourcePath = program.sourcePath;
    request.entryPoint = std::string(entryPoint);
    request.stage = stage;
    request.macros = dispatch.macros;
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
                              const LayoutResolver& resolveLayout, std::string* error) {
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
                                                  fx::FxShaderStage::vertex, compiler, entry);
                const auto pixel = compileShader(device, program, dispatch, raster->pixelShader,
                                                 fx::FxShaderStage::fragment, compiler, entry);
                entry.pipeline = device.createGraphicsPipelineEx({.layout = *layout, .shaders = {vertex, pixel}});
                break;
            }
            case fx::FxOpKind::postprocess:
                throw std::invalid_argument("postprocess FX pipeline needs a renderer-owned fullscreen vertex shader: " +
                                            dispatch.name);
            case fx::FxOpKind::compute: {
                const auto* compute = std::get_if<fx::FxComputeDispatch>(&dispatch.executable);
                if (compute == nullptr || compute->computeShader.empty())
                    throw std::invalid_argument("compute FX pass requires a compute shader: " + dispatch.name);
                const auto shader = compileShader(device, program, dispatch, compute->computeShader,
                                                  fx::FxShaderStage::compute, compiler, entry);
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
                                  fx::FxShaderStage::rayGeneration, compiler, entry));
                for (const auto& shader : ray->missShaders)
                    descriptor.miss.push_back(
                        compileShader(device, program, dispatch, shader, fx::FxShaderStage::miss, compiler, entry));
                for (const auto& group : ray->hitGroups) {
                    RayTracingHitGroupDesc hit;
                    hit.type = group.type == core::fx::FxRayTracingHitGroupType::procedural
                                   ? RayTracingHitGroupType::procedural
                                   : RayTracingHitGroupType::triangles;
                    if (!group.closestHit.empty())
                        hit.closestHit = compileShader(device, program, dispatch, group.closestHit,
                                                       fx::FxShaderStage::closestHit, compiler, entry);
                    if (!group.anyHit.empty())
                        hit.anyHit = compileShader(device, program, dispatch, group.anyHit,
                                                   fx::FxShaderStage::anyHit, compiler, entry);
                    if (!group.intersection.empty())
                        hit.intersection = compileShader(device, program, dispatch, group.intersection,
                                                         fx::FxShaderStage::intersection, compiler, entry);
                    descriptor.hitGroups.push_back(hit);
                }
                for (const auto& shader : ray->callableShaders)
                    descriptor.callable.push_back(compileShader(device, program, dispatch, shader,
                                                                fx::FxShaderStage::callable, compiler, entry));
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
