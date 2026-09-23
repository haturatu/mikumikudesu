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
    if (name == "D24_UNORM_S8_UINT" || name == "D24S8")
        return PixelFormat::depth24Stencil8;
    throw std::invalid_argument("FX resource format is unsupported: " + std::string(value));
}

[[nodiscard]] SamplerResourceDesc samplerDesc(const core::EffectSampler& sampler) {
    SamplerResourceDesc result;
    result.filter = sampler.filterKind == core::FxFilter::point ? SamplerFilter::nearest : SamplerFilter::linear;
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
    result.addressW = address(sampler.addressW);
    result.maxAnisotropy = sampler.maxAnisotropy;
    result.comparison = static_cast<SamplerCompareOp>(sampler.comparisonFunc);
    result.borderColor = static_cast<SamplerBorderColor>(sampler.borderColor);
    result.mipLodBias = sampler.mipLodBias;
    result.minLod = sampler.minLod;
    result.maxLod = sampler.maxLod;
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

[[nodiscard]] std::string sizeConversion(std::string_view base, std::string_view conversion) {
    if (conversion == "one")
        return "1";
    std::string result;
    const bool scalarBase =
        base == "VERTEXCOUNT" || base == "CLONEDVERTEXCOUNT" || base == "TOTALMATERIAL" || base == "TOTALMATERIALCOUNT";
    for (const auto axis : std::array<char, 3>{'x', 'y', 'z'}) {
        const bool containsAxis = std::ranges::any_of(
            conversion, [axis](unsigned char character) { return static_cast<char>(std::tolower(character)) == axis; });
        if (!containsAxis)
            continue;
        if (!result.empty())
            result += '*';
        if (scalarBase && axis == 'x')
            result += base;
        else if (scalarBase)
            result += '1';
        else
            result += std::string(base) + '.' + axis;
    }
    if (result.empty())
        throw std::invalid_argument("unsupported YRZFX size conversion: " + std::string(conversion));
    return result;
}

[[nodiscard]] core::fx::FxSizeExpr sizeExpression(const core::EffectSize& source, std::uint32_t dimension,
                                                  bool defaultToRenderTarget) {
    core::fx::FxSizeExpr result;
    result.base = source.absolute ? std::string{} : source.base;
    result.dimension =
        source.dimension != 0 ? source.dimension : (!source.base.empty() && !source.absolute ? 0U : dimension);
    result.widthRatio = source.absolute ? 1.0F : source.widthRatio;
    result.heightRatio = source.absolute ? 1.0F : source.heightRatio;
    result.depthRatio = source.absolute ? 1.0F : source.depthRatio;
    if (source.absolute || source.rounding == "trunc")
        result.rounding = core::fx::FxSizeExpr::Rounding::truncate;
    else if (source.rounding == "round")
        result.rounding = core::fx::FxSizeExpr::Rounding::nearest;
    else if (source.rounding == "ceil")
        result.rounding = core::fx::FxSizeExpr::Rounding::ceil;
    else
        throw std::invalid_argument("unsupported YRZFX size rounding mode: " + source.rounding);
    if (source.absolute && source.width != 0)
        result.xExpr = std::to_string(source.width);
    if (source.absolute && source.height != 0)
        result.yExpr = std::to_string(source.height);
    if (source.absolute && source.depth != 0)
        result.zExpr = std::to_string(source.depth);
    if (!source.absolute) {
        if (result.base.empty() && defaultToRenderTarget)
            result.base = "DEFAULT_RTSIZE";
        if (!result.base.empty()) {
            if (result.xExpr.empty())
                result.xExpr = sizeConversion(result.base, source.convX);
            if ((result.dimension == 0 || result.dimension >= 2) && result.yExpr.empty())
                result.yExpr = sizeConversion(result.base, source.convY);
            if ((result.dimension == 0 || result.dimension >= 3) && result.zExpr.empty())
                result.zExpr = sizeConversion(result.base, source.convZ);
        } else if (result.xExpr.empty()) {
            result.xExpr = "1";
            if (result.dimension >= 2)
                result.yExpr = "1";
            if (result.dimension >= 3)
                result.zExpr = "1";
        }
    }
    return result;
}

[[nodiscard]] core::fx::FxExtent resolveFxExtent(const core::EffectSize& source, std::uint32_t dimension,
                                                 bool defaultToRenderTarget, const fx::FxFrameContext& context,
                                                 const ExtentTable& table) {
    const auto expression = sizeExpression(source, dimension, defaultToRenderTarget);
    const auto evaluated = evaluationContext(context);
    return core::fx::FxSizeResolver{}.resolve(expression, evaluated, table);
}

struct FxTextureUsageSummary {
    bool sampled{};
    bool storage{};
    bool colorAttachment{};
    bool depthAttachment{};
};

[[nodiscard]] FxTextureUsageSummary summarizeTextureUsage(const fx::FxProgram& program, std::string_view name) {
    FxTextureUsageSummary result;
    for (const auto& dispatch : program.passes) {
        for (const auto& use : dispatch.resources) {
            if (use.name != name)
                continue;
            switch (use.role) {
            case fx::FxResourceRole::sampled:
                result.sampled |= !use.write;
                break;
            case fx::FxResourceRole::storage:
                result.storage |= use.write;
                break;
            case fx::FxResourceRole::colorAttachment:
                result.colorAttachment |= use.write;
                break;
            case fx::FxResourceRole::depthAttachment:
                result.depthAttachment |= use.write;
                break;
            }
        }
    }
    return result;
}

[[nodiscard]] ResourceUsage textureUsage(std::string_view view, PixelFormat format,
                                         const FxTextureUsageSummary& summary) {
    ResourceUsage usage = ResourceUsage::transferSrc | ResourceUsage::transferDst;
    if (isDepthFormat(format) || summary.depthAttachment || contains(view, "DSV") || contains(view, "DEPTH")) {
        usage |= ResourceUsage::depthRead | ResourceUsage::depthWrite;
    }
    if (summary.sampled || (!isDepthFormat(format) && !contains(view, "DSV") && !contains(view, "DEPTH"))) {
        usage |= ResourceUsage::sampledRead;
    }
    if (summary.storage || contains(view, "UAV") || contains(view, "STORAGE"))
        usage |= ResourceUsage::storageReadWrite;
    if (summary.colorAttachment || contains(view, "RTV") || contains(view, "COLOR"))
        usage |= ResourceUsage::colorAttachment;
    return usage;
}

[[nodiscard]] DescriptorKind textureDescriptorKind(std::string_view view, PixelFormat format) {
    if (!isDepthFormat(format) && (contains(view, "UAV") || contains(view, "STORAGE")))
        return DescriptorKind::storageImage;
    return DescriptorKind::sampledImage;
}

[[nodiscard]] ResourceUsage bufferUsage(const fx::FxProgram& program, std::string_view name, std::string_view view) {
    ResourceUsage usage = ResourceUsage::transferDst;
    // StructuredBuffer and RWStructuredBuffer both use storage-buffer
    // descriptors; readonly affects shader access, not the descriptor class.
    usage |= ResourceUsage::storageReadWrite;
    for (const auto& dispatch : program.passes) {
        const auto* raster = std::get_if<fx::FxRasterDispatch>(&dispatch.executable);
        if (raster == nullptr)
            continue;
        if (raster->vertexBuffer == name)
            usage |= ResourceUsage::vertexRead;
        if (raster->indexBuffer == name)
            usage |= ResourceUsage::indexRead;
    }
    static_cast<void>(view);
    return usage;
}

[[nodiscard]] DescriptorKind bufferDescriptorKind(std::string_view) {
    return DescriptorKind::storageBuffer;
}

[[nodiscard]] std::size_t checkedSize(std::uint64_t value, std::string_view name) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        throw std::overflow_error("FX resource size is too large: " + std::string(name));
    return static_cast<std::size_t>(value);
}

[[nodiscard]] bool hasExplicitSize(const core::EffectSize& size) noexcept {
    return size.absolute || !size.base.empty() || size.dimension != 0 || size.widthRatio != 1.0F ||
           size.heightRatio != 1.0F || size.depthRatio != 1.0F || size.convX != "x" || size.convY != "y" ||
           size.convZ != "z" || size.rounding != "trunc";
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

bool FxResourceStore::add(Resource resource) {
    if (resource.name.empty() || indices_.contains(resource.name))
        return false;
    indices_.emplace(resource.name, resources_.size());
    resources_.push_back(std::move(resource));
    return true;
}

void FxResourceStore::clear() noexcept {
    resources_.clear();
    indices_.clear();
}

const FxResourceStore::Resource* FxResourceStore::find(std::string_view name) const noexcept {
    const auto found = indices_.find(std::string(name));
    return found == indices_.end() ? nullptr : &resources_[found->second];
}

FxResourceStore::Resource* FxResourceStore::find(std::string_view name) noexcept {
    const auto found = indices_.find(std::string(name));
    return found == indices_.end() ? nullptr : &resources_[found->second];
}

FxPassDescriptorRuntime::~FxPassDescriptorRuntime() {
    reset();
}

bool FxPassDescriptorRuntime::initialize(Device& device, const fx::FxProgram& program, std::uint32_t resourceSet,
                                         const FxResourceStore& store, std::string* error) {
    if (error != nullptr)
        error->clear();
    reset();
    device_ = &device;
    try {
        const auto stages = allFxStages();
        for (const auto& dispatch : program.passes) {
            Entry entry;
            entry.plan = fx::planPassBindings(program, dispatch, resourceSet);
            if (entry.plan.bindings.empty()) {
                entries_.emplace(dispatch.name, std::move(entry));
                continue;
            }
            DescriptorSetLayoutDesc layout;
            layout.bindings.reserve(entry.plan.bindings.size());
            std::vector<DescriptorBindingEx> bindings;
            bindings.reserve(entry.plan.bindings.size());
            for (const auto& binding : entry.plan.bindings) {
                DescriptorKind kind = DescriptorKind::sampledImage;
                switch (binding.descriptorClass) {
                case fx::FxDescriptorClass::sampledImage:
                    kind = DescriptorKind::sampledImage;
                    break;
                case fx::FxDescriptorClass::storageImage:
                    kind = DescriptorKind::storageImage;
                    break;
                case fx::FxDescriptorClass::storageBuffer:
                    kind = DescriptorKind::storageBuffer;
                    break;
                case fx::FxDescriptorClass::sampler:
                    kind = DescriptorKind::sampler;
                    break;
                case fx::FxDescriptorClass::accelerationStructure:
                    kind = DescriptorKind::accelerationStructure;
                    break;
                }
                layout.bindings.push_back({binding.binding, kind, binding.count, stages});
                const auto* physical = store.find(binding.resource);
                if (physical == nullptr)
                    throw std::invalid_argument("FX pass binding has no physical resource: " + binding.resource);
                DescriptorBindingEx descriptor{.slot = binding.binding, .arrayElement = 0};
                if (physical->kind == FxResourceStore::Kind::texture)
                    descriptor.texture = physical->texture;
                else if (physical->kind == FxResourceStore::Kind::buffer)
                    descriptor.buffer = physical->buffer;
                else
                    descriptor.sampler = physical->sampler;
                bindings.push_back(descriptor);
            }
            entry.layout = device.createDescriptorSetLayoutEx(layout);
            if (!entry.layout.valid())
                throw std::runtime_error("FX pass descriptor layout is invalid: " + dispatch.name);
            entry.set = device.allocateDescriptorSetEx(entry.layout, bindings);
            if (!entry.set.valid())
                throw std::runtime_error("FX pass descriptor set is invalid: " + dispatch.name);
            entries_.emplace(dispatch.name, std::move(entry));
        }
    } catch (const std::exception& exception) {
        setError(error, exception.what());
        reset();
        return false;
    } catch (...) {
        setError(error, "FX pass descriptor runtime initialization failed");
        reset();
        return false;
    }
    return true;
}

void FxPassDescriptorRuntime::reset() noexcept {
    Device* device = device_;
    if (device != nullptr) {
        try {
            device->waitIdle();
        } catch (...) {
        }
        for (const auto& [name, entry] : entries_) {
            static_cast<void>(name);
            if (entry.set.valid()) {
                try {
                    device->destroyDescriptorSetEx(entry.set);
                } catch (...) {
                }
            }
            if (entry.layout.valid()) {
                try {
                    device->destroyDescriptorSetLayoutEx(entry.layout);
                } catch (...) {
                }
            }
        }
    }
    entries_.clear();
    device_ = nullptr;
}

std::optional<handles::DescriptorSetHandle>
FxPassDescriptorRuntime::resolveDescriptorSet(const fx::FxDispatch& dispatch) const {
    const auto found = entries_.find(dispatch.name);
    if (found == entries_.end() || !found->second.set.valid())
        return std::nullopt;
    return found->second.set;
}

std::optional<handles::DescriptorSetLayoutHandle>
FxPassDescriptorRuntime::resolveDescriptorLayout(const fx::FxDispatch& dispatch) const {
    const auto found = entries_.find(dispatch.name);
    if (found == entries_.end() || !found->second.layout.valid())
        return std::nullopt;
    return found->second.layout;
}

const fx::FxPassBindingPlan* FxPassDescriptorRuntime::bindingPlan(const fx::FxDispatch& dispatch) const noexcept {
    const auto found = entries_.find(dispatch.name);
    return found == entries_.end() ? nullptr : &found->second.plan;
}

FxResourceRuntime::~FxResourceRuntime() {
    reset();
}

bool FxResourceRuntime::initialize(Device& device, const fx::FxProgram& program, const fx::FxFrameContext& context,
                                   std::string* error, std::uint32_t resourceSet) {
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
            if (store_.find(name) != nullptr)
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
            const auto resolvedFx =
                external.has_value() && !hasExplicitSize(declaration.size)
                    ? core::fx::FxExtent{.x = external->width, .y = external->height, .z = 1, .dimension = 2}
                    : resolveFxExtent(declaration.size, 2, true, context, table);
            const Extent3D resolved{resolvedFx.x, resolvedFx.y, resolvedFx.z};
            if (external.has_value() &&
                (resolved.width != external->width || resolved.height != external->height || resolved.depth != 1))
                throw std::invalid_argument("FX external texture extent does not match its declaration: " + name);
            const auto levels = mipLevels(resolved, declaration.mipmap);
            const auto binding = nextBinding(registerClass(declaration.view));
            const auto usageSummary = summarizeTextureUsage(program, name);
            TextureResourceDesc description{
                .dimension = TextureDimension::d2,
                .extent = resolved,
                .format = format,
                .mipLevels = levels,
                .arrayLayers = 1,
                .usage = textureUsage(declaration.view, format, usageSummary),
                .lifetime = ResourceLifetime::persistent,
            };
            reserveBytes(static_cast<std::uint64_t>(estimateTextureBytes(description)), name);
            FxResourceStore::Resource resource{.name = name,
                                               .kind = FxResourceStore::Kind::texture,
                                               .texture = {},
                                               .buffer = {},
                                               .sampler = {},
                                               .extent = resolved,
                                               .format = format,
                                               .legacyDescriptorKind = textureDescriptorKind(declaration.view, format),
                                               .legacyBinding = binding};
            resource.texture = device.createTextureEx(description);
            if (!resource.texture.valid())
                throw std::runtime_error("FX texture allocation returned an invalid handle: " + name);
            if (external.has_value()) {
                device.uploadTextureEx(resource.texture, external->pixels, 0, 0);
                if (levels > 1)
                    device.generateMipmapsEx(resource.texture);
            }
            if (!store_.add(std::move(resource)))
                throw std::invalid_argument("FX resource declaration is duplicated: " + name);
            table.add(name, resolvedFx);
            const auto* stored = store_.find(name);
            addBinding(stored->legacyBinding, stored->legacyDescriptorKind);
        }
        for (const auto& declaration : program.textures3D) {
            const auto name = addName(declaration.name);
            if (!declaration.filename.empty())
                throw std::invalid_argument("FX external 3D textures are not supported by this loader: " + name);
            const auto format = pixelFormat(declaration.format);
            const auto resolvedFx = resolveFxExtent(declaration.size, 3, false, context, table);
            const Extent3D resolved{resolvedFx.x, resolvedFx.y, resolvedFx.z};
            const auto levels = mipLevels(resolved, declaration.mipmap);
            const auto binding = nextBinding(registerClass(declaration.view));
            const auto usageSummary = summarizeTextureUsage(program, name);
            TextureResourceDesc description{
                .dimension = TextureDimension::d3,
                .extent = resolved,
                .format = format,
                .mipLevels = levels,
                .arrayLayers = 1,
                .usage = textureUsage(declaration.view, format, usageSummary),
                .lifetime = ResourceLifetime::persistent,
            };
            reserveBytes(static_cast<std::uint64_t>(estimateTextureBytes(description)), name);
            FxResourceStore::Resource resource{.name = name,
                                               .kind = FxResourceStore::Kind::texture,
                                               .texture = {},
                                               .buffer = {},
                                               .sampler = {},
                                               .extent = resolved,
                                               .format = format,
                                               .legacyDescriptorKind = textureDescriptorKind(declaration.view, format),
                                               .legacyBinding = binding};
            resource.texture = device.createTextureEx(description);
            if (!resource.texture.valid())
                throw std::runtime_error("FX 3D texture allocation returned an invalid handle: " + name);
            if (!store_.add(std::move(resource)))
                throw std::invalid_argument("FX resource declaration is duplicated: " + name);
            table.add(name, resolvedFx);
            const auto* stored = store_.find(name);
            addBinding(stored->legacyBinding, stored->legacyDescriptorKind);
        }
        for (const auto& declaration : program.buffers) {
            const auto name = addName(declaration.name);
            if (declaration.elementSize == 0)
                throw std::invalid_argument("FX buffer element size is zero: " + name);
            auto bufferSize = declaration.size;
            if (!bufferSize.absolute && bufferSize.base.empty()) {
                bufferSize.base =
                    program.category == core::fx::FxCategory::deform ? "CLONEDVERTEXCOUNT" : "DEFAULT_RTSIZE";
                bufferSize.dimension = program.category == core::fx::FxCategory::deform ? 1U : 2U;
            }
            const auto resolvedFx = resolveFxExtent(bufferSize, 1, false, context, table);
            const Extent3D resolved{resolvedFx.x, resolvedFx.y, resolvedFx.z};
            const auto bytes = core::fx::FxSizeResolver::bufferBytes(resolvedFx, declaration.elementSize);
            reserveBytes(bytes, name);
            const auto binding = nextBinding(registerClass(declaration.view));
            BufferResourceDesc description{
                .size = checkedSize(bytes, name),
                .usage = bufferUsage(program, declaration.name, declaration.view),
                .cpuVisible = false,
                .lifetime = ResourceLifetime::persistent,
            };
            FxResourceStore::Resource resource{.name = name,
                                               .kind = FxResourceStore::Kind::buffer,
                                               .texture = {},
                                               .buffer = {},
                                               .sampler = {},
                                               .extent = resolved,
                                               .format = PixelFormat::rgba8Unorm,
                                               .legacyDescriptorKind = bufferDescriptorKind(declaration.view),
                                               .legacyBinding = binding};
            resource.buffer = device.createBufferEx(description);
            if (!resource.buffer.valid())
                throw std::runtime_error("FX buffer allocation returned an invalid handle: " + name);
            if (!store_.add(std::move(resource)))
                throw std::invalid_argument("FX resource declaration is duplicated: " + name);
            table.add(name, resolvedFx);
            const auto* stored = store_.find(name);
            addBinding(stored->legacyBinding, stored->legacyDescriptorKind);
        }
        for (const auto& declaration : program.samplers) {
            const auto name = addName(declaration.name);
            const auto binding = nextBinding(NativeSceneRegisterClass::sampler);
            FxResourceStore::Resource resource{.name = name,
                                               .kind = FxResourceStore::Kind::sampler,
                                               .texture = {},
                                               .buffer = {},
                                               .sampler = {},
                                               .extent = {},
                                               .format = PixelFormat::rgba8Unorm,
                                               .legacyDescriptorKind = DescriptorKind::sampler,
                                               .legacyBinding = binding};
            resource.sampler = device.createSamplerEx(samplerDesc(declaration));
            if (!resource.sampler.valid())
                throw std::runtime_error("FX sampler allocation returned an invalid handle: " + name);
            if (!store_.add(std::move(resource)))
                throw std::invalid_argument("FX resource declaration is duplicated: " + name);
            const auto* stored = store_.find(name);
            addBinding(stored->legacyBinding, stored->legacyDescriptorKind);
        }

        if (!descriptorLayoutDesc_.bindings.empty()) {
            descriptorLayout_ = device.createDescriptorSetLayoutEx(descriptorLayoutDesc_);
            if (!descriptorLayout_.valid())
                throw std::runtime_error("FX resource descriptor layout is invalid");
            std::vector<DescriptorBindingEx> bindings;
            bindings.reserve(store_.size());
            for (const auto& resource : store_.resources()) {
                DescriptorBindingEx binding{.slot = resource.legacyBinding, .arrayElement = 0};
                if (resource.kind == FxResourceStore::Kind::texture)
                    binding.texture = resource.texture;
                else if (resource.kind == FxResourceStore::Kind::buffer)
                    binding.buffer = resource.buffer;
                else
                    binding.sampler = resource.sampler;
                bindings.push_back(binding);
            }
            descriptorSet_ = device.allocateDescriptorSetEx(descriptorLayout_, bindings);
            if (!descriptorSet_.valid())
                throw std::runtime_error("FX resource descriptor set is invalid");
        }
        if (!passDescriptors_.initialize(device, program, resourceSet, store_, error))
            throw std::runtime_error(error != nullptr && !error->empty() ? *error
                                                                         : "FX pass descriptors are unavailable");
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
        passDescriptors_.reset();
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
        for (const auto& resource : store_.resources()) {
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
    store_.clear();
    descriptorLayoutDesc_ = {};
    descriptorLayout_ = {};
    descriptorSet_ = {};
}

std::optional<handles::TextureHandle> FxResourceRuntime::resolveTexture(std::string_view name) const {
    const auto* resource = store_.find(name);
    if (resource == nullptr || resource->kind != FxResourceStore::Kind::texture)
        return std::nullopt;
    return resource->texture;
}

std::optional<handles::BufferHandle> FxResourceRuntime::resolveBuffer(std::string_view name) const {
    const auto* resource = store_.find(name);
    if (resource == nullptr || resource->kind != FxResourceStore::Kind::buffer)
        return std::nullopt;
    return resource->buffer;
}

std::optional<handles::SamplerHandle> FxResourceRuntime::resolveSampler(std::string_view name) const {
    const auto* resource = store_.find(name);
    if (resource == nullptr || resource->kind != FxResourceStore::Kind::sampler)
        return std::nullopt;
    return resource->sampler;
}

std::optional<handles::DescriptorSetHandle> FxResourceRuntime::resolveDescriptorSet(const fx::FxDispatch&) const {
    if (!descriptorSet_.valid())
        return std::nullopt;
    return descriptorSet_;
}

std::optional<Extent3D> FxResourceRuntime::extent(std::string_view name) const {
    const auto* resource = store_.find(name);
    if (resource == nullptr || resource->kind == FxResourceStore::Kind::sampler)
        return std::nullopt;
    return resource->extent;
}

std::optional<FxResourceRuntime::ResolvedTexture>
FxResourceRuntime::resolveOutputTexture(std::span<const fx::FxDispatch> ordered) const {
    for (const auto& dispatch : std::ranges::reverse_view(ordered)) {
        for (const auto& resource : std::ranges::reverse_view(dispatch.resources)) {
            if (!resource.write)
                continue;
            const auto* candidate = store_.find(resource.name);
            if (candidate == nullptr || candidate->kind != FxResourceStore::Kind::texture)
                continue;
            return ResolvedTexture{
                .handle = candidate->texture, .extent = candidate->extent, .format = candidate->format};
        }
    }
    return std::nullopt;
}

} // namespace dayo::graphics
