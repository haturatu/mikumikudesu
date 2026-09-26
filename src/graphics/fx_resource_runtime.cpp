#include "graphics/fx_resource_runtime.hpp"

#include "core/fx/fx_size.hpp"
#include "core/image.hpp"
#include "fx/fx_resource_sizes.hpp"
#include "graphics/fx_material_gpu_runtime.hpp"
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

struct SharedAllocation {
    Device* device;
    handles::TextureHandle texture;
    handles::BufferHandle buffer;
    SharedAllocation(Device& owner, handles::TextureHandle image, handles::BufferHandle data)
        : device(&owner), texture(image), buffer(data) {}
    ~SharedAllocation() {
        try {
            device->waitIdle();
            if (texture.valid())
                device->destroyTextureEx(texture);
            if (buffer.valid())
                device->destroyBufferEx(buffer);
        } catch (...) {
        }
    }
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
    return parsePixelFormat(upper(value));
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
    ResourceUsage usage = ResourceUsage::transferDst | ResourceUsage::transferSrc;
    // StructuredBuffer and RWStructuredBuffer both use storage-buffer
    // descriptors; readonly affects shader access, not the descriptor class.
    usage |= ResourceUsage::storageReadWrite;
    const auto sharedSource = std::ranges::any_of(program.buffers, [name](const auto& declaration) {
        return declaration.name == name && fxSharedMode(declaration.shared, "source");
    });
    if (sharedSource) {
        // A shared buffer can be consumed as raster geometry by another FX.
        // Include both roles before allocation so a later shared=ref never
        // needs to add usage flags to the physical buffer.
        usage |= ResourceUsage::vertexRead | ResourceUsage::indexRead;
    }
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

[[nodiscard]] bool isDdsPath(const std::filesystem::path& path) {
    return upper(path.extension().string()) == ".DDS";
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

[[nodiscard]] DescriptorKind descriptorKind(fx::FxDescriptorClass descriptorClass) noexcept {
    switch (descriptorClass) {
    case fx::FxDescriptorClass::sampledImage:
        return DescriptorKind::sampledImage;
    case fx::FxDescriptorClass::storageImage:
        return DescriptorKind::storageImage;
    case fx::FxDescriptorClass::storageBuffer:
        return DescriptorKind::storageBuffer;
    case fx::FxDescriptorClass::sampler:
        return DescriptorKind::sampler;
    case fx::FxDescriptorClass::accelerationStructure:
        return DescriptorKind::accelerationStructure;
    }
    return DescriptorKind::sampledImage;
}

void appendDescriptorWrites(std::vector<DescriptorBindingEx>& output, const fx::FxLogicalBinding& logical,
                            const FxResourceStore& store, const FxMaterialGpuBindings* material) {
    const auto append = [&output, &logical](std::uint32_t element, handles::TextureHandle texture,
                                            handles::BufferHandle buffer, handles::SamplerHandle sampler,
                                            handles::AccelerationStructureHandle accelerationStructure) {
        DescriptorBindingEx binding{.slot = logical.binding, .arrayElement = element};
        binding.texture = texture;
        binding.buffer = buffer;
        binding.sampler = sampler;
        binding.accelerationStructure = accelerationStructure;
        output.push_back(binding);
    };
    if (logical.resource.starts_with("@matdesc/")) {
        if (material == nullptr)
            throw std::logic_error("FX MatDesc descriptor has no GPU material bindings");
        if (logical.resource == "@matdesc/idx")
            append(0, {}, material->materialIndices, {}, {});
        else if (logical.resource == "@matdesc/tex")
            append(0, {}, material->textureIndices2D, {}, {});
        else if (logical.resource == "@matdesc/tex3D")
            append(0, {}, material->textureIndices3D, {}, {});
        else if (logical.resource == "@matdesc/value")
            append(0, {}, material->values, {}, {});
        else if (logical.resource == "@matdesc/texture2D") {
            if (material->textures2D.size() != logical.count)
                throw std::logic_error("MatDesc 2D texture catalog changed descriptor count; "
                                       "reinitialize the FX descriptor runtime");
            for (std::uint32_t element = 0; element < logical.count; ++element)
                append(element, material->textures2D[element], {}, {}, {});
        } else if (logical.resource == "@matdesc/texture3D") {
            if (material->textures3D.size() != logical.count)
                throw std::logic_error("MatDesc 3D texture catalog changed descriptor count; "
                                       "reinitialize the FX descriptor runtime");
            for (std::uint32_t element = 0; element < logical.count; ++element)
                append(element, material->textures3D[element], {}, {}, {});
        } else {
            throw std::logic_error("unknown MatDesc logical descriptor: " + logical.resource);
        }
        return;
    }

    const auto* physical = store.find(logical.resource);
    if (physical == nullptr)
        throw std::invalid_argument("FX pass binding has no physical resource: " + logical.resource);
    if (physical->kind == FxResourceStore::Kind::texture)
        append(0, physical->texture, {}, {}, {});
    else if (physical->kind == FxResourceStore::Kind::buffer)
        append(0, {}, physical->buffer, {}, {});
    else
        append(0, {}, {}, physical->sampler, {});
}

[[nodiscard]] bool sameDescriptorWrites(std::span<const DescriptorBindingEx> left,
                                        std::span<const DescriptorBindingEx> right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto& a = left[index];
        const auto& b = right[index];
        if (a.slot != b.slot || a.arrayElement != b.arrayElement || a.buffer != b.buffer || a.texture != b.texture ||
            a.sampler != b.sampler || a.accelerationStructure != b.accelerationStructure || a.mipLevel != b.mipLevel)
            return false;
    }
    return true;
}

} // namespace

bool fxSharedMode(std::string_view value, std::string_view mode) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return upper(value) == upper(mode);
}

bool FxResourceRuntime::sharedReferencesChanged(const fx::FxProgram& program) const {
    const auto changed = [this](const auto& declarations) {
        return std::ranges::any_of(declarations, [this](const auto& declaration) {
            if (!fxSharedMode(declaration.shared, "ref"))
                return false;
            const auto source = sharedResolver_ ? sharedResolver_(declaration.name) : std::nullopt;
            const auto* old = store_.find(declaration.name);
            return !source || old == nullptr || source->ownership != old->ownership ||
                   source->texture != old->texture || source->buffer != old->buffer;
        });
    };
    return changed(program.textures) || changed(program.textures3D) || changed(program.buffers);
}

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
                                         const FxResourceStore& store, std::string* error,
                                         const FxMaterialGpuRuntime* materialRuntime) {
    if (error != nullptr)
        error->clear();
    reset();
    device_ = &device;
    store_ = &store;
    materialRuntime_ = materialRuntime;
    try {
        const auto stages = allFxStages();
        for (const auto& dispatch : program.passes) {
            const auto [entryIt, inserted] = entries_.try_emplace(dispatch.name);
            if (!inserted)
                throw std::invalid_argument("FX pass names must be unique for descriptor planning: " + dispatch.name);
            auto& entry = entryIt->second;
            entry.plan = fx::planPassBindings(program, dispatch, resourceSet);
            if (entry.plan.material.has_value() &&
                (materialRuntime_ == nullptr || !materialRuntime_->ready() || !materialRuntime_->belongsTo(device)))
                throw std::invalid_argument("FX MatDesc pass requires a ready GPU material runtime on the same device");
            if (entry.plan.bindings.empty())
                continue;

            const auto initialMaterialBindings = entry.plan.material.has_value()
                                                     ? materialRuntime_->bindings(device.currentFrameSlot())
                                                     : FxMaterialGpuBindings{};
            for (auto& binding : entry.plan.bindings) {
                if (binding.resource == "@matdesc/texture2D") {
                    if (initialMaterialBindings.textures2D.size() > std::numeric_limits<std::uint32_t>::max())
                        throw std::overflow_error("MatDesc 2D texture descriptor array is too large");
                    binding.count = static_cast<std::uint32_t>(initialMaterialBindings.textures2D.size());
                } else if (binding.resource == "@matdesc/texture3D") {
                    if (initialMaterialBindings.textures3D.size() > std::numeric_limits<std::uint32_t>::max())
                        throw std::overflow_error("MatDesc 3D texture descriptor array is too large");
                    binding.count = static_cast<std::uint32_t>(initialMaterialBindings.textures3D.size());
                }
                if (binding.count == 0)
                    throw std::invalid_argument("FX descriptor array cannot be empty: " + binding.resource);
            }

            std::vector<std::uint32_t> setIndices;
            for (const auto& binding : entry.plan.bindings)
                if (std::ranges::find(setIndices, binding.set) == setIndices.end())
                    setIndices.push_back(binding.set);
            std::ranges::sort(setIndices);
            entry.sets.reserve(setIndices.size());
            for (const auto setIndex : setIndices) {
                DescriptorSetLayoutDesc layout;
                std::vector<const fx::FxLogicalBinding*> logicalBindings;
                for (const auto& binding : entry.plan.bindings) {
                    if (binding.set != setIndex)
                        continue;
                    if (std::ranges::any_of(layout.bindings, [&binding](const auto& existing) {
                            return existing.binding == binding.binding;
                        }))
                        throw std::invalid_argument("FX descriptor binding collision in set " +
                                                    std::to_string(setIndex) + ": " + std::to_string(binding.binding));
                    logicalBindings.push_back(&binding);
                    layout.bindings.push_back(
                        {binding.binding, descriptorKind(binding.descriptorClass), binding.count, stages});
                }
                Entry::Set set;
                set.index = setIndex;
                set.layout = device.createDescriptorSetLayoutEx(layout);
                if (!set.layout.valid())
                    throw std::runtime_error("FX pass descriptor layout is invalid: " + dispatch.name);
                entry.sets.push_back(std::move(set));
                auto& ownedSet = entry.sets.back();
                const auto frameCount = entry.plan.material.has_value() ? kNativeFramesInFlight : 1U;
                ownedSet.handles.reserve(frameCount);
                ownedSet.bindings.reserve(frameCount);
                for (std::uint32_t frameSlot = 0; frameSlot < frameCount; ++frameSlot) {
                    const auto materialBindings = entry.plan.material.has_value()
                                                      ? materialRuntime_->bindings(frameSlot)
                                                      : FxMaterialGpuBindings{};
                    std::vector<DescriptorBindingEx> writes;
                    for (const auto* logical : logicalBindings)
                        appendDescriptorWrites(writes, *logical, store,
                                               entry.plan.material.has_value() ? &materialBindings : nullptr);
                    const auto descriptorSet = device.allocateDescriptorSetEx(ownedSet.layout, writes);
                    if (!descriptorSet.valid())
                        throw std::runtime_error("FX pass descriptor set is invalid: " + dispatch.name);
                    ownedSet.handles.push_back(descriptorSet);
                    ownedSet.bindings.push_back(std::move(writes));
                }
            }
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
            for (const auto& set : entry.sets) {
                for (const auto handle : set.handles) {
                    if (!handle.valid())
                        continue;
                    try {
                        device->destroyDescriptorSetEx(handle);
                    } catch (...) {
                    }
                }
                if (set.layout.valid()) {
                    try {
                        device->destroyDescriptorSetLayoutEx(set.layout);
                    } catch (...) {
                    }
                }
            }
        }
    }
    entries_.clear();
    store_ = nullptr;
    materialRuntime_ = nullptr;
    device_ = nullptr;
}

std::optional<handles::DescriptorSetHandle>
FxPassDescriptorRuntime::resolveDescriptorSet(const fx::FxDispatch& dispatch) const {
    const auto sets = descriptorSets(dispatch);
    if (sets.empty() || !sets.front().set.valid())
        return std::nullopt;
    return sets.front().set;
}

std::optional<handles::DescriptorSetLayoutHandle>
FxPassDescriptorRuntime::resolveDescriptorLayout(const fx::FxDispatch& dispatch) const {
    const auto layouts = descriptorLayouts(dispatch);
    if (layouts.empty() || !layouts.front().second.valid())
        return std::nullopt;
    return layouts.front().second;
}

std::vector<FxPassDescriptorSet> FxPassDescriptorRuntime::descriptorSets(const fx::FxDispatch& dispatch) const {
    const auto found = entries_.find(dispatch.name);
    if (found == entries_.end())
        return {};
    const auto hasMaterial = found->second.plan.material.has_value();
    const auto slot = hasMaterial && device_ != nullptr ? device_->currentFrameSlot() % kNativeFramesInFlight : 0U;
    std::vector<FxPassDescriptorSet> result;
    result.reserve(found->second.sets.size());
    for (auto& set : found->second.sets) {
        if (set.handles.empty())
            continue;
        if (hasMaterial) {
            if (device_ == nullptr || store_ == nullptr || materialRuntime_ == nullptr || !materialRuntime_->ready() ||
                !materialRuntime_->belongsTo(*device_))
                throw std::logic_error("FX MatDesc GPU runtime is no longer ready on its owning device");
            const auto materialBindings = materialRuntime_->bindings(slot);
            std::vector<DescriptorBindingEx> currentWrites;
            for (const auto& logical : found->second.plan.bindings)
                if (logical.set == set.index)
                    appendDescriptorWrites(currentWrites, logical, *store_, &materialBindings);
            if (!sameDescriptorWrites(currentWrites, set.bindings[slot])) {
                device_->updateDescriptorSetEx(set.handles[slot], currentWrites);
                set.bindings[slot] = std::move(currentWrites);
            }
        }
        result.push_back({.setIndex = set.index, .layout = set.layout, .set = set.handles[slot]});
    }
    return result;
}

std::vector<std::pair<std::uint32_t, handles::DescriptorSetLayoutHandle>>
FxPassDescriptorRuntime::descriptorLayouts(const fx::FxDispatch& dispatch) const {
    const auto found = entries_.find(dispatch.name);
    if (found == entries_.end())
        return {};
    std::vector<std::pair<std::uint32_t, handles::DescriptorSetLayoutHandle>> result;
    result.reserve(found->second.sets.size());
    for (const auto& set : found->second.sets)
        result.emplace_back(set.index, set.layout);
    return result;
}

const fx::FxPassBindingPlan* FxPassDescriptorRuntime::bindingPlan(const fx::FxDispatch& dispatch) const noexcept {
    const auto found = entries_.find(dispatch.name);
    return found == entries_.end() ? nullptr : &found->second.plan;
}

FxResourceRuntime::~FxResourceRuntime() {
    reset();
}

bool FxResourceRuntime::initialize(Device& device, const fx::FxProgram& program, const fx::FxFrameContext& context,
                                   std::string* error, std::uint32_t resourceSet,
                                   const FxMaterialGpuRuntime* materialRuntime,
                                   const FxMaterialRuntimeInitializer& initializeMaterialRuntime) {
    if (error != nullptr)
        error->clear();
    reset();
    device_ = &device;
    try {
        const auto borrow = [this](const auto& declarations, FxResourceStore::Kind kind, std::uint32_t dimension) {
            for (const auto& declaration : declarations) {
                if (!fxSharedMode(declaration.shared, "ref"))
                    continue;
                auto source = sharedResolver_ ? sharedResolver_(declaration.name) : std::nullopt;
                if (!source)
                    throw std::invalid_argument("unresolved shared=ref: " + declaration.name);
                if (!source->ownership)
                    throw std::invalid_argument("shared source has no lifetime owner: " + declaration.name);
                if (source->kind != kind || (kind == FxResourceStore::Kind::texture && source->dimension != dimension))
                    throw std::invalid_argument("shared resource kind/dimension mismatch: " + declaration.name);
                source->name = declaration.name;
                if (!store_.add(std::move(*source)))
                    throw std::invalid_argument("duplicate shared reference: " + declaration.name);
            }
        };
        borrow(program.textures, FxResourceStore::Kind::texture, 2);
        borrow(program.textures3D, FxResourceStore::Kind::texture, 3);
        borrow(program.buffers, FxResourceStore::Kind::buffer, 1);
        fx::FxResourceSizeTable table(program, context, this);
        table.resolveAll();
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

        const auto bindReference = [&](std::string_view name, std::string_view view, ResourceUsage required) {
            auto* source = store_.find(name);
            if (source == nullptr)
                throw std::logic_error("shared resource was not resolved");
            if ((toBits(source->usage) & toBits(required)) != toBits(required))
                throw std::invalid_argument("shared source usage cannot satisfy ref: " + std::string(name));
            source->legacyBinding = nextBinding(registerClass(view));
            source->legacyDescriptorKind = source->kind == FxResourceStore::Kind::texture
                                               ? textureDescriptorKind(view, source->format)
                                               : bufferDescriptorKind(view);
            addBinding(source->legacyBinding, source->legacyDescriptorKind);
        };

        for (const auto& declaration : program.textures) {
            if (fxSharedMode(declaration.shared, "ref")) {
                const auto* source = store_.find(declaration.name);
                bindReference(
                    declaration.name, declaration.view,
                    textureUsage(declaration.view, source->format, summarizeTextureUsage(program, declaration.name)));
                continue;
            }

            const auto name = addName(declaration.name);
            std::optional<core::TextureImage> externalDds;
            if (!declaration.filename.empty()) {
                externalDds = core::loadTextureImage(externalPath(program, declaration.filename));
                if (externalDds->dimension != core::DdsDimension::twoD || externalDds->arrayLayers != 1)
                    throw std::invalid_argument("FX Texture2D external image must contain one 2D image: " + name);
            }
            const auto format = pixelFormat(externalDds ? externalDds->format : declaration.format);
            const auto resolvedFx = *table.find(name);
            const Extent3D resolved{resolvedFx.x, resolvedFx.y, resolvedFx.z};
            const auto levels = !declaration.mipmap ? 1U
                                                    : (externalDds.has_value() && externalDds->mipLevels > 1
                                                           ? externalDds->mipLevels
                                                           : mipLevels(resolved, true));
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
            const auto allocationBytes = static_cast<std::uint64_t>(estimateTextureBytes(description));
            reserveBytes(allocationBytes, name);
            FxResourceStore::Resource resource{.name = name,
                                               .kind = FxResourceStore::Kind::texture,
                                               .texture = {},
                                               .buffer = {},
                                               .sampler = {},
                                               .extent = resolved,
                                               .format = format,
                                               .dimension = 2,
                                               .allocationBytes = allocationBytes,
                                               .elementSize = 0,
                                               .elementType = {},
                                               .legacyDescriptorKind = textureDescriptorKind(declaration.view, format),
                                               .legacyBinding = binding};
            resource.usage = description.usage;
            resource.texture = device.createTextureEx(description);
            if (!resource.texture.valid())
                throw std::runtime_error("FX texture allocation returned an invalid handle: " + name);
            if (!store_.add(resource))
                throw std::invalid_argument("FX resource declaration is duplicated: " + name);
            if (externalDds.has_value()) {
                const auto uploadedLevels = std::min(levels, externalDds->mipLevels);
                for (std::uint32_t mip = 0; mip < uploadedLevels; ++mip)
                    device.uploadTextureEx(resource.texture, externalDds->subresource(mip).pixels, mip, 0);
                if (levels > uploadedLevels)
                    device.generateMipmapsEx(resource.texture);
            }
            const auto* stored = store_.find(name);
            addBinding(stored->legacyBinding, stored->legacyDescriptorKind);
        }
        for (const auto& declaration : program.textures3D) {
            if (fxSharedMode(declaration.shared, "ref")) {
                const auto* source = store_.find(declaration.name);
                bindReference(
                    declaration.name, declaration.view,
                    textureUsage(declaration.view, source->format, summarizeTextureUsage(program, declaration.name)));
                continue;
            }

            const auto name = addName(declaration.name);
            std::optional<core::TextureImage> externalDds;
            if (!declaration.filename.empty()) {
                const auto path = externalPath(program, declaration.filename);
                if (!isDdsPath(path))
                    throw std::invalid_argument("FX external 3D texture must use DDS: " + name);
                externalDds = core::loadTextureImage(path);
                if (externalDds->dimension != core::DdsDimension::threeD || externalDds->arrayLayers != 1)
                    throw std::invalid_argument("FX Texture3D external DDS must contain one volume: " + name);
            }
            const auto format = pixelFormat(externalDds ? externalDds->format : declaration.format);
            const auto resolvedFx = externalDds.has_value() ? core::fx::FxExtent{.x = externalDds->width,
                                                                                 .y = externalDds->height,
                                                                                 .z = externalDds->depth,
                                                                                 .dimension = 3}
                                                            : *table.find(name);
            const Extent3D resolved{resolvedFx.x, resolvedFx.y, resolvedFx.z};
            const auto levels = !declaration.mipmap ? 1U
                                                    : (externalDds.has_value() && externalDds->mipLevels > 1
                                                           ? externalDds->mipLevels
                                                           : mipLevels(resolved, true));
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
            const auto allocationBytes = static_cast<std::uint64_t>(estimateTextureBytes(description));
            reserveBytes(allocationBytes, name);
            FxResourceStore::Resource resource{.name = name,
                                               .kind = FxResourceStore::Kind::texture,
                                               .texture = {},
                                               .buffer = {},
                                               .sampler = {},
                                               .extent = resolved,
                                               .format = format,
                                               .dimension = 3,
                                               .allocationBytes = allocationBytes,
                                               .elementSize = 0,
                                               .elementType = {},
                                               .legacyDescriptorKind = textureDescriptorKind(declaration.view, format),
                                               .legacyBinding = binding};
            resource.usage = description.usage;
            resource.texture = device.createTextureEx(description);
            if (!resource.texture.valid())
                throw std::runtime_error("FX 3D texture allocation returned an invalid handle: " + name);
            if (!store_.add(resource))
                throw std::invalid_argument("FX resource declaration is duplicated: " + name);
            if (externalDds.has_value()) {
                const auto uploadedLevels = std::min(levels, externalDds->mipLevels);
                for (std::uint32_t mip = 0; mip < uploadedLevels; ++mip)
                    device.uploadTextureEx(resource.texture, externalDds->subresource(mip).pixels, mip, 0);
                if (levels > uploadedLevels)
                    device.generateMipmapsEx(resource.texture);
            }
            const auto* stored = store_.find(name);
            addBinding(stored->legacyBinding, stored->legacyDescriptorKind);
        }
        for (const auto& declaration : program.buffers) {
            if (fxSharedMode(declaration.shared, "ref")) {
                const auto* source = store_.find(declaration.name);
                if ((declaration.elementSize != 0 && declaration.elementSize != source->elementSize) ||
                    (!declaration.type.empty() && declaration.type != source->elementType))
                    throw std::invalid_argument("shared buffer stride/type mismatch: " + declaration.name);
                bindReference(declaration.name, declaration.view,
                              bufferUsage(program, declaration.name, declaration.view));
                continue;
            }

            const auto name = addName(declaration.name);
            if (declaration.elementSize == 0)
                throw std::invalid_argument("FX buffer element size is zero: " + name);
            const auto resolvedFx = *table.find(name);
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
                                               .dimension = resolvedFx.dimension,
                                               .allocationBytes = bytes,
                                               .elementSize = declaration.elementSize,
                                               .elementType = declaration.type,
                                               .legacyDescriptorKind = bufferDescriptorKind(declaration.view),
                                               .legacyBinding = binding};
            resource.usage = description.usage;
            resource.buffer = device.createBufferEx(description);
            if (!resource.buffer.valid())
                throw std::runtime_error("FX buffer allocation returned an invalid handle: " + name);
            if (!store_.add(std::move(resource)))
                throw std::invalid_argument("FX resource declaration is duplicated: " + name);
            const auto* stored = store_.find(name);
            addBinding(stored->legacyBinding, stored->legacyDescriptorKind);
        }
        const auto ownSources = [&](const auto& declarations) {
            for (const auto& declaration : declarations) {
                if (!fxSharedMode(declaration.shared, "source"))
                    continue;
                auto* resource = store_.find(declaration.name);
                if (resource == nullptr)
                    throw std::logic_error("shared source allocation is missing");
                const auto texture = resource->texture;
                const auto buffer = resource->buffer;
                // The device outlives its FX runtimes and registry. The final reference
                // waits for submitted work before releasing the physical allocation.
                resource->ownership = std::make_shared<SharedAllocation>(device, texture, buffer);
            }
        };
        ownSources(program.textures);
        ownSources(program.textures3D);
        ownSources(program.buffers);

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
                                               .dimension = 0,
                                               .allocationBytes = 0,
                                               .elementSize = 0,
                                               .elementType = {},
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
        if (initializeMaterialRuntime) {
            materialRuntime = initializeMaterialRuntime(store_, error);
            if (materialRuntime == nullptr)
                throw std::runtime_error(error != nullptr && !error->empty()
                                             ? *error
                                             : "FX material runtime initialization returned no runtime");
        }
        if (!passDescriptors_.initialize(device, program, resourceSet, store_, error, materialRuntime))
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
            if (resource.ownership)
                continue;
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

std::optional<core::fx::FxExtent> FxResourceRuntime::find(std::string_view name) const {
    const auto* resource = store_.find(name);
    if (resource == nullptr || resource->kind == FxResourceStore::Kind::sampler)
        return std::nullopt;
    return core::fx::FxExtent{.x = resource->extent.width,
                              .y = resource->extent.height,
                              .z = resource->extent.depth,
                              .dimension = resource->dimension};
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
