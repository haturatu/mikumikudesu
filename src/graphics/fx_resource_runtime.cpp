#include "graphics/fx_resource_runtime.hpp"

#include "core/fx/fx_size.hpp"
#include "core/image.hpp"
#include "graphics/native_scene_bindings.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace dayo::graphics {
namespace {

class ExtentTable final : public core::fx::FxResourceTable {
  public:
    void add(std::string name, core::fx::FxExtent extent) {
        values_.insert_or_assign(std::move(name), extent);
    }

    [[nodiscard]] std::optional<core::fx::FxExtent> find(std::string_view name) const override {
        const auto found = values_.find(std::string(name));
        if (found == values_.end())
            return std::nullopt;
        return found->second;
    }

  private:
    std::unordered_map<std::string, core::fx::FxExtent> values_;
};

[[nodiscard]] std::string upper(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value)
        result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
    return result;
}

[[nodiscard]] bool contains(std::string_view value, std::string_view needle) {
    return upper(value).find(upper(needle)) != std::string::npos;
}

[[nodiscard]] PixelFormat pixelFormat(std::string_view value) {
    const auto name = upper(value);
    if (name.empty() || name == "R8G8B8A8_UNORM")
        return PixelFormat::rgba8Unorm;
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
    if (name == "R16G16B16A16_FLOAT")
        return PixelFormat::rgba16Float;
    if (name == "R32G32B32A32_FLOAT")
        return PixelFormat::rgba32Float;
    if (name == "D32_FLOAT")
        return PixelFormat::depth32Float;
    throw std::invalid_argument("FX resource format is unsupported: " + std::string(value));
}

[[nodiscard]] SamplerResourceDesc samplerDesc(const core::EffectSampler& sampler) {
    SamplerResourceDesc result;
    const auto filter = upper(sampler.filter);
    result.filter = filter == "POINT" || filter == "NEAREST" ? SamplerFilter::nearest : SamplerFilter::linear;
    const auto address = [](std::string_view value) {
        const auto mode = upper(value);
        if (mode == "CLAMP" || mode == "CLAMP_TO_EDGE")
            return SamplerAddressMode::clampToEdge;
        if (mode == "MIRROR" || mode == "MIRRORED_REPEAT")
            return SamplerAddressMode::mirroredRepeat;
        if (mode == "BORDER" || mode == "CLAMP_TO_BORDER")
            return SamplerAddressMode::clampToBorder;
        return SamplerAddressMode::repeat;
    };
    result.addressU = address(sampler.addressU);
    result.addressV = address(sampler.addressV);
    result.addressW = result.addressV;
    return result;
}

[[nodiscard]] core::fx::FxEvalContext evaluationContext(const fx::FxFrameContext& context) {
    const auto checked = [](std::size_t value, std::string_view name) {
        if (value > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::overflow_error("FX resource context is too large: " + std::string(name));
        return static_cast<std::int64_t>(value);
    };
    core::fx::FxEvalContext result;
    result.rtWidth = context.renderWidth;
    result.rtHeight = context.renderHeight;
    result.vertexCount = checked(context.vertexCount, "vertexCount");
    result.totalMaterial = checked(context.totalMaterial, "totalMaterial");
    result.modelIndex = context.modelIndex;
    result.cloneCount = context.cloneCount;
    result.clonedVertexCount = checked(context.clonedVertexCount, "clonedVertexCount");
    result.frameIndex = static_cast<std::int64_t>(context.frame);
    result.sampleIndex = checked(static_cast<std::size_t>(context.sample), "sampleIndex");
    return result;
}

[[nodiscard]] core::fx::FxSizeExpr sizeExpression(const core::EffectSize& source, std::uint32_t dimension,
                                                  bool defaultToRenderTarget) {
    core::fx::FxSizeExpr result;
    result.base = source.base;
    result.dimension = dimension;
    result.widthRatio = source.widthRatio;
    result.heightRatio = source.heightRatio;
    if (source.width != 0)
        result.xExpr = std::to_string(source.width);
    if (source.height != 0)
        result.yExpr = std::to_string(source.height);
    if (source.depth != 0)
        result.zExpr = std::to_string(source.depth);
    if (result.base.empty() && result.xExpr.empty() && defaultToRenderTarget)
        result.base = "DEFAULT_RTSIZE";
    if (result.base.empty() && result.xExpr.empty())
        result.xExpr = "1";
    return result;
}

[[nodiscard]] Extent3D resolveExtent(const core::EffectSize& source, std::uint32_t dimension,
                                     bool defaultToRenderTarget, const fx::FxFrameContext& context,
                                     const ExtentTable& table) {
    const auto expression = sizeExpression(source, dimension, defaultToRenderTarget);
    const auto evaluated = evaluationContext(context);
    const auto extent = core::fx::FxSizeResolver{}.resolve(expression, evaluated, table);
    return {.width = extent.x, .height = extent.y, .depth = extent.z};
}

[[nodiscard]] ResourceUsage textureUsage(std::string_view view, PixelFormat format) {
    ResourceUsage usage = ResourceUsage::transferSrc | ResourceUsage::transferDst;
    if (isDepthFormat(format) || contains(view, "DSV") || contains(view, "DEPTH")) {
        usage |= ResourceUsage::depthRead | ResourceUsage::depthWrite;
    } else {
        // Keep declarations descriptor-compatible even when a resource is
        // also used as an RTV. Pass usage still controls the image layout;
        // this bit only makes the typed sampled-image view legal.
        usage |= ResourceUsage::sampledRead;
        if (contains(view, "UAV") || contains(view, "STORAGE"))
            usage |= ResourceUsage::storageReadWrite;
        if (contains(view, "RTV") || contains(view, "COLOR"))
            usage |= ResourceUsage::colorAttachment;
    }
    return usage;
}

[[nodiscard]] DescriptorKind textureDescriptorKind(std::string_view view, PixelFormat format) {
    if (!isDepthFormat(format) && (contains(view, "UAV") || contains(view, "STORAGE")))
        return DescriptorKind::storageImage;
    return DescriptorKind::sampledImage;
}

[[nodiscard]] ResourceUsage bufferUsage(std::string_view view) {
    ResourceUsage usage = ResourceUsage::transferDst;
    if (contains(view, "UAV") || contains(view, "STORAGE"))
        usage |= ResourceUsage::storageReadWrite;
    else
        usage |= ResourceUsage::uniformRead;
    return usage;
}

[[nodiscard]] DescriptorKind bufferDescriptorKind(std::string_view view) {
    return contains(view, "UAV") || contains(view, "STORAGE") ? DescriptorKind::storageBuffer
                                                              : DescriptorKind::uniformBuffer;
}

[[nodiscard]] std::size_t checkedSize(std::uint64_t value, std::string_view name) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        throw std::overflow_error("FX resource size is too large: " + std::string(name));
    return static_cast<std::size_t>(value);
}

[[nodiscard]] bool hasExplicitSize(const core::EffectSize& size) noexcept {
    return size.absolute || !size.base.empty() || size.width != 0 || size.height != 0 || size.depth != 0;
}

[[nodiscard]] std::uint32_t mipLevels(Extent3D extent, bool enabled) noexcept {
    if (!enabled)
        return 1;
    std::uint32_t levels = 1;
    auto largest = std::max({extent.width, extent.height, extent.depth});
    while (largest > 1) {
        largest /= 2;
        ++levels;
    }
    return levels;
}

[[nodiscard]] std::filesystem::path externalPath(const fx::FxProgram& program, std::string_view filename) {
    const std::filesystem::path path(filename);
    return path.is_absolute() ? path : program.sourcePath.parent_path() / path;
}

[[nodiscard]] ShaderStageMask allFxStages() noexcept {
    return ShaderStageMask::vertex | ShaderStageMask::fragment | ShaderStageMask::compute |
           ShaderStageMask::rayGeneration | ShaderStageMask::miss | ShaderStageMask::closestHit |
           ShaderStageMask::anyHit | ShaderStageMask::intersection | ShaderStageMask::callable;
}

void setError(std::string* error, std::string message) {
    if (error != nullptr)
        *error = std::move(message);
}

} // namespace

FxResourceRuntime::~FxResourceRuntime() {
    reset();
}

bool FxResourceRuntime::initialize(Device& device, const fx::FxProgram& program, const fx::FxFrameContext& context,
                                   std::string* error) {
    if (error != nullptr)
        error->clear();
    reset();
    device_ = &device;
    try {
        ExtentTable table;
        const auto stages = allFxStages();
        std::uint64_t totalBytes = 0;
        constexpr auto maxBytes = core::fx::kMaxTransientBytesPerEffect;
        const auto reserveBytes = [&](std::uint64_t bytes, std::string_view name) {
            if (bytes > maxBytes || totalBytes > maxBytes - bytes)
                throw std::overflow_error("FX resource allocation budget exceeded: " + std::string(name));
            totalBytes += bytes;
        };
        const auto addName = [&](std::string name) {
            if (name.empty())
                throw std::invalid_argument("FX resource declaration has an empty name");
            if (!indices_.emplace(name, resources_.size()).second)
                throw std::invalid_argument("FX resource declaration is duplicated: " + name);
            return name;
        };
        std::uint32_t uavBinding = 0;
        std::uint32_t sampledBinding = 0;
        std::uint32_t samplerBinding = 0;
        const auto nextBinding = [&](NativeSceneRegisterClass registerClass) {
            const auto index = registerClass == NativeSceneRegisterClass::uav       ? uavBinding++
                               : registerClass == NativeSceneRegisterClass::sampled ? sampledBinding++
                                                                                    : samplerBinding++;
            return nativeSceneBinding(registerClass, index);
        };
        const auto registerClass = [](std::string_view view) {
            return contains(view, "UAV") || contains(view, "STORAGE") ? NativeSceneRegisterClass::uav
                                                                      : NativeSceneRegisterClass::sampled;
        };
        const auto addBinding = [&](std::uint32_t binding, DescriptorKind kind) {
            descriptorLayoutDesc_.bindings.push_back({binding, kind, 1, stages});
        };

        for (const auto& declaration : program.textures) {
            const auto name = addName(declaration.name);
            const auto format = pixelFormat(declaration.format);
            std::optional<core::ImageRgba8> external;
            if (!declaration.filename.empty()) {
                if (format != PixelFormat::rgba8Unorm)
                    throw std::invalid_argument("FX external texture format must be RGBA8_UNORM: " + name);
                external = core::loadImageRgba8(externalPath(program, declaration.filename));
            }
            const auto resolved = external.has_value() && !hasExplicitSize(declaration.size)
                                      ? Extent3D{external->width, external->height, 1}
                                      : resolveExtent(declaration.size, 2, true, context, table);
            if (external.has_value() &&
                (resolved.width != external->width || resolved.height != external->height || resolved.depth != 1))
                throw std::invalid_argument("FX external texture extent does not match its declaration: " + name);
            const auto levels = mipLevels(resolved, declaration.mipmap);
            const auto binding = nextBinding(registerClass(declaration.view));
            TextureResourceDesc description{
                .dimension = TextureDimension::d2,
                .extent = resolved,
                .format = format,
                .mipLevels = levels,
                .arrayLayers = 1,
                .usage = textureUsage(declaration.view, format),
                .lifetime = ResourceLifetime::persistent,
            };
            reserveBytes(static_cast<std::uint64_t>(estimateTextureBytes(description)), name);
            Resource resource{.name = name,
                              .kind = Kind::texture,
                              .descriptorKind = textureDescriptorKind(declaration.view, format),
                              .binding = binding,
                              .extent = resolved,
                              .format = format};
            resource.texture = device.createTextureEx(description);
            if (!resource.texture.valid())
                throw std::runtime_error("FX texture allocation returned an invalid handle: " + name);
            if (external.has_value()) {
                device.uploadTextureEx(resource.texture, external->pixels, 0, 0);
                if (levels > 1)
                    device.generateMipmapsEx(resource.texture);
            }
            resources_.push_back(resource);
            table.add(name, {.x = resolved.width, .y = resolved.height, .z = resolved.depth, .dimension = 2});
            addBinding(resource.binding, resource.descriptorKind);
        }
        for (const auto& declaration : program.textures3D) {
            const auto name = addName(declaration.name);
            if (!declaration.filename.empty())
                throw std::invalid_argument("FX external 3D textures are not supported by this loader: " + name);
            const auto format = pixelFormat(declaration.format);
            const auto resolved = resolveExtent(declaration.size, 3, false, context, table);
            const auto levels = mipLevels(resolved, declaration.mipmap);
            const auto binding = nextBinding(registerClass(declaration.view));
            TextureResourceDesc description{
                .dimension = TextureDimension::d3,
                .extent = resolved,
                .format = format,
                .mipLevels = levels,
                .arrayLayers = 1,
                .usage = textureUsage(declaration.view, format),
                .lifetime = ResourceLifetime::persistent,
            };
            reserveBytes(static_cast<std::uint64_t>(estimateTextureBytes(description)), name);
            Resource resource{.name = name,
                              .kind = Kind::texture,
                              .descriptorKind = textureDescriptorKind(declaration.view, format),
                              .binding = binding,
                              .extent = resolved,
                              .format = format};
            resource.texture = device.createTextureEx(description);
            if (!resource.texture.valid())
                throw std::runtime_error("FX 3D texture allocation returned an invalid handle: " + name);
            resources_.push_back(resource);
            table.add(name, {.x = resolved.width, .y = resolved.height, .z = resolved.depth, .dimension = 3});
            addBinding(resource.binding, resource.descriptorKind);
        }
        for (const auto& declaration : program.buffers) {
            const auto name = addName(declaration.name);
            if (declaration.elementSize == 0)
                throw std::invalid_argument("FX buffer element size is zero: " + name);
            const auto resolved = resolveExtent(declaration.size, 1, false, context, table);
            const auto bytes = core::fx::FxSizeResolver::bufferBytes(
                {.x = resolved.width, .y = 1, .z = 1, .dimension = 1}, declaration.elementSize);
            reserveBytes(bytes, name);
            const auto binding = nextBinding(registerClass(declaration.view));
            BufferResourceDesc description{
                .size = checkedSize(bytes, name),
                .usage = bufferUsage(declaration.view),
                .cpuVisible = false,
                .lifetime = ResourceLifetime::persistent,
            };
            Resource resource{.name = name,
                              .kind = Kind::buffer,
                              .descriptorKind = bufferDescriptorKind(declaration.view),
                              .binding = binding,
                              .extent = resolved};
            resource.buffer = device.createBufferEx(description);
            if (!resource.buffer.valid())
                throw std::runtime_error("FX buffer allocation returned an invalid handle: " + name);
            resources_.push_back(resource);
            table.add(name, {.x = resolved.width, .y = 1, .z = 1, .dimension = 1});
            addBinding(resource.binding, resource.descriptorKind);
        }
        for (const auto& declaration : program.samplers) {
            const auto name = addName(declaration.name);
            const auto binding = nextBinding(NativeSceneRegisterClass::sampler);
            Resource resource{
                .name = name, .kind = Kind::sampler, .descriptorKind = DescriptorKind::sampler, .binding = binding};
            resource.sampler = device.createSamplerEx(samplerDesc(declaration));
            if (!resource.sampler.valid())
                throw std::runtime_error("FX sampler allocation returned an invalid handle: " + name);
            resources_.push_back(resource);
            addBinding(resource.binding, resource.descriptorKind);
        }

        if (!descriptorLayoutDesc_.bindings.empty()) {
            descriptorLayout_ = device.createDescriptorSetLayoutEx(descriptorLayoutDesc_);
            if (!descriptorLayout_.valid())
                throw std::runtime_error("FX resource descriptor layout is invalid");
            std::vector<DescriptorBindingEx> bindings;
            bindings.reserve(resources_.size());
            for (const auto& resource : resources_) {
                DescriptorBindingEx binding{.slot = resource.binding, .arrayElement = 0};
                if (resource.kind == Kind::texture)
                    binding.texture = resource.texture;
                else if (resource.kind == Kind::buffer)
                    binding.buffer = resource.buffer;
                else
                    binding.sampler = resource.sampler;
                bindings.push_back(binding);
            }
            descriptorSet_ = device.allocateDescriptorSetEx(descriptorLayout_, bindings);
            if (!descriptorSet_.valid())
                throw std::runtime_error("FX resource descriptor set is invalid");
        }
    } catch (const std::exception& exception) {
        setError(error, exception.what());
        reset();
        return false;
    } catch (...) {
        setError(error, "FX resource runtime initialization failed");
        reset();
        return false;
    }
    return true;
}

void FxResourceRuntime::reset() noexcept {
    Device* device = device_;
    if (device != nullptr) {
        try {
            device->waitIdle();
        } catch (...) {
        }
        if (descriptorSet_.valid()) {
            try {
                device->destroyDescriptorSetEx(descriptorSet_);
            } catch (...) {
            }
        }
        if (descriptorLayout_.valid()) {
            try {
                device->destroyDescriptorSetLayoutEx(descriptorLayout_);
            } catch (...) {
            }
        }
        for (const auto& resource : resources_) {
            try {
                if (resource.texture.valid())
                    device->destroyTextureEx(resource.texture);
                if (resource.buffer.valid())
                    device->destroyBufferEx(resource.buffer);
                if (resource.sampler.valid())
                    device->destroySamplerEx(resource.sampler);
            } catch (...) {
            }
        }
    }
    device_ = nullptr;
    resources_.clear();
    indices_.clear();
    descriptorLayoutDesc_ = {};
    descriptorLayout_ = {};
    descriptorSet_ = {};
}

std::optional<handles::TextureHandle> FxResourceRuntime::resolveTexture(std::string_view name) const {
    const auto found = indices_.find(std::string(name));
    if (found == indices_.end() || resources_[found->second].kind != Kind::texture)
        return std::nullopt;
    return resources_[found->second].texture;
}

std::optional<handles::BufferHandle> FxResourceRuntime::resolveBuffer(std::string_view name) const {
    const auto found = indices_.find(std::string(name));
    if (found == indices_.end() || resources_[found->second].kind != Kind::buffer)
        return std::nullopt;
    return resources_[found->second].buffer;
}

std::optional<handles::SamplerHandle> FxResourceRuntime::resolveSampler(std::string_view name) const {
    const auto found = indices_.find(std::string(name));
    if (found == indices_.end() || resources_[found->second].kind != Kind::sampler)
        return std::nullopt;
    return resources_[found->second].sampler;
}

std::optional<handles::DescriptorSetHandle> FxResourceRuntime::resolveDescriptorSet(const fx::FxDispatch&) const {
    if (!descriptorSet_.valid())
        return std::nullopt;
    return descriptorSet_;
}

std::optional<Extent3D> FxResourceRuntime::extent(std::string_view name) const {
    const auto found = indices_.find(std::string(name));
    if (found == indices_.end() || resources_[found->second].kind == Kind::sampler)
        return std::nullopt;
    return resources_[found->second].extent;
}

std::optional<FxResourceRuntime::ResolvedTexture>
FxResourceRuntime::resolveOutputTexture(std::span<const fx::FxDispatch> ordered) const {
    for (const auto& dispatch : std::ranges::reverse_view(ordered)) {
        for (const auto& resource : std::ranges::reverse_view(dispatch.resources)) {
            if (!resource.write)
                continue;
            const auto found = indices_.find(resource.name);
            if (found == indices_.end())
                continue;
            const auto& candidate = resources_[found->second];
            if (candidate.kind != Kind::texture)
                continue;
            return ResolvedTexture{.handle = candidate.texture, .extent = candidate.extent, .format = candidate.format};
        }
    }
    return std::nullopt;
}

} // namespace dayo::graphics
