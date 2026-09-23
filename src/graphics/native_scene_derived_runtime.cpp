#include "graphics/native_scene_derived_runtime.hpp"

#include "graphics/native_scene_data.hpp"
#include "graphics/resource.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace dayo::graphics {
namespace {

void setError(std::string* error, std::string value) {
    if (error != nullptr)
        *error = std::move(value);
}

[[nodiscard]] bool hasRgbaPixels(const core::ImageRgba8& image) noexcept {
    if (image.width == 0 || image.height == 0)
        return false;
    const auto width = static_cast<std::size_t>(image.width);
    const auto height = static_cast<std::size_t>(image.height);
    if (width > std::numeric_limits<std::size_t>::max() / height)
        return false;
    const auto pixels = width * height;
    return pixels <= std::numeric_limits<std::size_t>::max() / 4U && image.pixels.size() == pixels * 4U;
}

[[nodiscard]] std::size_t nonEmptyBytes(std::size_t bytes) noexcept {
    return std::max<std::size_t>(bytes, sizeof(std::uint32_t));
}

[[nodiscard]] std::array<std::size_t, 6> tableByteSizes(const NativeSceneDerivedData& data) {
    return {nonEmptyBytes(checkedResourceMul(data.modelToMaterial.size(), sizeof(std::array<std::uint32_t, 2>))),
            nonEmptyBytes(checkedResourceMul(data.materialToModel.size(), sizeof(std::uint32_t))),
            nonEmptyBytes(checkedResourceMul(data.peekaboo.size(), sizeof(std::int32_t))),
            nonEmptyBytes(checkedResourceMul(data.materialSelected.size(), sizeof(std::int32_t))),
            nonEmptyBytes(checkedResourceMul(data.cloneCount.size(), sizeof(std::uint32_t))),
            nonEmptyBytes(checkedResourceMul(data.textureTable.size(), sizeof(std::uint32_t)))};
}

template <typename T> void uploadVector(Device& device, handles::BufferHandle buffer, const std::vector<T>& values) {
    if (values.empty()) {
        const std::uint32_t zero{};
        device.uploadBufferEx(buffer, std::as_bytes(std::span<const std::uint32_t>(&zero, 1)), 0);
        return;
    }
    device.uploadBufferEx(buffer, std::as_bytes(std::span<const T>(values)), 0);
}

} // namespace

NativeSceneDerivedData makeNativeSceneDerivedData(std::span<const NativeSceneDerivedModel> models) {
    NativeSceneDerivedData result;
    const auto modelSlots = std::max<std::size_t>(models.size(), 1U);
    result.modelToMaterial.reserve(modelSlots);
    result.peekaboo.reserve(modelSlots);
    result.cloneCount.reserve(modelSlots);
    result.textureTable.reserve(modelSlots);

    std::size_t totalMaterials = 0;
    for (const auto& model : models) {
        if (model.materialCount > std::numeric_limits<std::size_t>::max() - totalMaterials)
            throw std::overflow_error("native scene material table size overflow");
        totalMaterials += model.materialCount;
    }
    if (totalMaterials > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("native scene material table exceeds 32-bit model indices");
    result.materialToModel.reserve(std::max<std::size_t>(totalMaterials, 1U));
    result.materialSelected.reserve(std::max<std::size_t>(totalMaterials, 1U));

    std::uint32_t materialBase = 0;
    for (std::size_t modelIndex = 0; modelIndex < models.size(); ++modelIndex) {
        const auto& model = models[modelIndex];
        if (modelIndex > std::numeric_limits<std::uint32_t>::max())
            throw std::overflow_error("native scene model table exceeds 32-bit model indices");
        result.modelToMaterial.push_back({materialBase, model.materialCount});
        result.peekaboo.push_back(model.visible ? 1 : 0);
        result.cloneCount.push_back(std::max(model.cloneCount, 1U));
        result.textureTable.push_back(model.textureBase);
        for (std::uint32_t materialIndex = 0; materialIndex < model.materialCount; ++materialIndex) {
            result.materialToModel.push_back(static_cast<std::uint32_t>(modelIndex));
            result.materialSelected.push_back(
                model.selectedMaterial >= 0 && static_cast<std::uint32_t>(model.selectedMaterial) == materialIndex ? 1
                                                                                                                   : 0);
        }
        materialBase += model.materialCount;
    }
    if (models.empty()) {
        result.modelToMaterial.push_back({0, 0});
        result.peekaboo.push_back(0);
        result.cloneCount.push_back(1);
        result.textureTable.push_back(0);
    }
    if (result.materialToModel.empty()) {
        result.materialToModel.push_back(0);
        result.materialSelected.push_back(0);
    }
    return result;
}

NativeSceneDerivedRuntime::~NativeSceneDerivedRuntime() {
    reset();
}

bool NativeSceneDerivedRuntime::initialize(Device& device, std::span<const core::ImageRgba8> images,
                                           std::string* error) {
    if (error != nullptr)
        error->clear();
    reset();
    try {
        device_ = &device;
        fallbackTexture_ = device.createTextureEx({
            .dimension = TextureDimension::d2,
            .extent = {1, 1, 1},
            .format = PixelFormat::rgba8Unorm,
            .mipLevels = 1,
            .arrayLayers = 1,
            .usage = ResourceUsage::sampledRead | ResourceUsage::transferDst,
            .lifetime = ResourceLifetime::persistent,
        });
        if (!fallbackTexture_.valid())
            throw std::runtime_error("native scene fallback texture allocation returned an invalid handle");
        constexpr std::array<std::uint8_t, 4> white{255, 255, 255, 255};
        device.uploadTextureEx(fallbackTexture_, white, 0, 0);
        textures_.reserve(std::max<std::size_t>(images.size(), 1U));
        for (const auto& image : images) {
            if (!hasRgbaPixels(image)) {
                textures_.push_back(fallbackTexture_);
                continue;
            }
            const auto texture = device.createTextureEx({
                .dimension = TextureDimension::d2,
                .extent = {image.width, image.height, 1},
                .format = PixelFormat::rgba8Unorm,
                .mipLevels = 1,
                .arrayLayers = 1,
                .usage = ResourceUsage::sampledRead | ResourceUsage::transferDst,
                .lifetime = ResourceLifetime::persistent,
            });
            if (!texture.valid())
                throw std::runtime_error("native scene PMX texture allocation returned an invalid handle");
            ownedTextures_.push_back(texture);
            textures_.push_back(texture);
            device.uploadTextureEx(texture, image.pixels, 0, 0);
        }
        if (textures_.empty())
            textures_.push_back(fallbackTexture_);
        data_ = makeNativeSceneDerivedData({});
        const auto initialCapacity = tableByteSizes(data_);
        for (auto& frame : frameBuffers_)
            frame.capacity = initialCapacity;
        if (!createTableBuffers(error))
            throw std::runtime_error(
                error != nullptr && !error->empty() ? *error : "native scene table buffer allocation failed");
    } catch (const std::exception& exception) {
        setError(error, std::string("native scene derived runtime initialization failed: ") + exception.what());
        reset();
        return false;
    } catch (...) {
        setError(error, "native scene derived runtime initialization failed");
        reset();
        return false;
    }
    return true;
}

bool NativeSceneDerivedRuntime::createTableBuffers(std::string* error) {
    if (device_ == nullptr) {
        setError(error, "native scene derived runtime has no device");
        return false;
    }
    try {
        for (auto& frame : frameBuffers_) {
            const auto create = [this](std::size_t size) {
                return device_->createBufferEx({.size = nonEmptyBytes(size),
                                                .usage = ResourceUsage::storageRead | ResourceUsage::transferDst,
                                                .cpuVisible = false,
                                                .lifetime = ResourceLifetime::persistent});
            };
            frame.modelToMaterial = create(frame.capacity[0]);
            frame.materialToModel = create(frame.capacity[1]);
            frame.peekaboo = create(frame.capacity[2]);
            frame.materialSelected = create(frame.capacity[3]);
            frame.cloneCount = create(frame.capacity[4]);
            frame.textureTable = create(frame.capacity[5]);
            if (!frame.modelToMaterial.valid() || !frame.materialToModel.valid() || !frame.peekaboo.valid() ||
                !frame.materialSelected.valid() || !frame.cloneCount.valid() || !frame.textureTable.valid())
                throw std::runtime_error("native scene table buffer allocation returned an invalid handle");
        }
    } catch (const std::exception& exception) {
        setError(error, std::string("native scene table buffer allocation failed: ") + exception.what());
        return false;
    } catch (...) {
        setError(error, "native scene table buffer allocation failed");
        return false;
    }
    return true;
}

bool NativeSceneDerivedRuntime::ensureTableBufferCapacity(const NativeSceneDerivedData& data, std::string* error) {
    if (frameBuffers_[0].modelToMaterial.valid()) {
        const auto required = tableByteSizes(data);
        const auto& capacity = frameBuffers_[0].capacity;
        bool fits = true;
        for (std::size_t index = 0; index < required.size(); ++index)
            fits = fits && required[index] <= capacity[index];
        if (fits)
            return true;
        try {
            device_->waitIdle();
        } catch (...) {
        }
        for (auto& frame : frameBuffers_) {
            const std::array buffers{frame.modelToMaterial,  frame.materialToModel, frame.peekaboo,
                                     frame.materialSelected, frame.cloneCount,      frame.textureTable};
            for (const auto buffer : buffers) {
                if (!buffer.valid())
                    continue;
                try {
                    device_->destroyBufferEx(buffer);
                } catch (...) {
                }
            }
            frame.modelToMaterial = {};
            frame.materialToModel = {};
            frame.peekaboo = {};
            frame.materialSelected = {};
            frame.cloneCount = {};
            frame.textureTable = {};
            frame.capacity = {};
        }
    }
    const auto required = tableByteSizes(data);
    for (auto& frame : frameBuffers_)
        frame.capacity = required;
    if (createTableBuffers(error))
        return true;
    for (auto& frame : frameBuffers_) {
        const std::array buffers{frame.modelToMaterial,  frame.materialToModel, frame.peekaboo,
                                 frame.materialSelected, frame.cloneCount,      frame.textureTable};
        for (const auto buffer : buffers) {
            if (!buffer.valid())
                continue;
            try {
                device_->destroyBufferEx(buffer);
            } catch (...) {
            }
        }
        frame.modelToMaterial = {};
        frame.materialToModel = {};
        frame.peekaboo = {};
        frame.materialSelected = {};
        frame.cloneCount = {};
        frame.textureTable = {};
        frame.capacity = {};
    }
    setError(error, error != nullptr && !error->empty() ? *error : "native scene table buffer allocation failed");
    return false;
}

bool NativeSceneDerivedRuntime::ensureOutputResources(Extent3D extent, std::string* error) {
    if (extent.width == 0 || extent.height == 0 || extent.depth != 1) {
        setError(error, "native scene output extent must be a non-empty 2D extent");
        return false;
    }
    const auto matches = [&extent](const FrameBuffers& frame) {
        return frame.outputExtent.width == extent.width && frame.outputExtent.height == extent.height &&
               frame.outputExtent.depth == extent.depth && frame.rtOutput.valid() && frame.oidnBuffer.valid() &&
               frame.normalDepth.valid() && frame.gbuffer1.valid() && frame.gbuffer2.valid();
    };
    if (matches(frameBuffers_[0]))
        return true;

    try {
        device_->waitIdle();
    } catch (...) {
    }
    for (auto& frame : frameBuffers_) {
        for (const auto texture : {frame.rtOutput, frame.normalDepth, frame.gbuffer1, frame.gbuffer2}) {
            if (!texture.valid())
                continue;
            try {
                device_->destroyTextureEx(texture);
            } catch (...) {
            }
        }
        if (frame.oidnBuffer.valid()) {
            try {
                device_->destroyBufferEx(frame.oidnBuffer);
            } catch (...) {
            }
        }
        frame.rtOutput = {};
        frame.oidnBuffer = {};
        frame.normalDepth = {};
        frame.gbuffer1 = {};
        frame.gbuffer2 = {};
        frame.outputExtent = {};
    }
    return createOutputResources(extent, error);
}

bool NativeSceneDerivedRuntime::createOutputResources(Extent3D extent, std::string* error) {
    try {
        const auto createTexture = [this, &extent](PixelFormat format, ResourceUsage usage) {
            return device_->createTextureEx({.dimension = TextureDimension::d2,
                                             .extent = extent,
                                             .format = format,
                                             .mipLevels = 1,
                                             .arrayLayers = 1,
                                             .usage = usage,
                                             .lifetime = ResourceLifetime::persistent});
        };
        const auto outputUsage = ResourceUsage::sampledRead | ResourceUsage::storageReadWrite |
                                 ResourceUsage::colorAttachment | ResourceUsage::transferSrc |
                                 ResourceUsage::transferDst;
        const auto storageTextureUsage = ResourceUsage::sampledRead | ResourceUsage::storageReadWrite |
                                         ResourceUsage::transferSrc | ResourceUsage::transferDst;
        const auto pixelCount =
            checkedResourceMul(static_cast<std::size_t>(extent.width), static_cast<std::size_t>(extent.height));
        const auto oidnBytes = checkedResourceMul(pixelCount, sizeof(NativeSceneOidnInput));
        constexpr std::array<float, 4> clearValue{};
        for (auto& frame : frameBuffers_) {
            frame.rtOutput = createTexture(PixelFormat::rgba16Float, outputUsage);
            frame.oidnBuffer = device_->createBufferEx(
                {.size = nonEmptyBytes(oidnBytes),
                 .usage = ResourceUsage::storageReadWrite | ResourceUsage::transferSrc | ResourceUsage::transferDst,
                 .cpuVisible = false,
                 .lifetime = ResourceLifetime::persistent});
            frame.normalDepth = createTexture(PixelFormat::rgba16Float, storageTextureUsage);
            frame.gbuffer1 = createTexture(PixelFormat::r32g32Uint, storageTextureUsage);
            frame.gbuffer2 = createTexture(PixelFormat::r32g32Float, storageTextureUsage);
            frame.outputExtent = extent;
            if (!frame.rtOutput.valid() || !frame.oidnBuffer.valid() || !frame.normalDepth.valid() ||
                !frame.gbuffer1.valid() || !frame.gbuffer2.valid())
                throw std::runtime_error("native scene output allocation returned an invalid handle");
            device_->clearTextureEx(frame.rtOutput, clearValue);
            device_->clearBufferEx(frame.oidnBuffer, 0);
            device_->clearTextureEx(frame.normalDepth, clearValue);
            device_->clearTextureEx(frame.gbuffer1, clearValue);
            device_->clearTextureEx(frame.gbuffer2, clearValue);
        }
    } catch (const std::exception& exception) {
        setError(error, std::string("native scene output allocation failed: ") + exception.what());
    } catch (...) {
        setError(error, "native scene output allocation failed");
    }
    const bool created = std::ranges::all_of(frameBuffers_, [](const FrameBuffers& frame) {
        return frame.rtOutput.valid() && frame.oidnBuffer.valid() && frame.normalDepth.valid() &&
               frame.gbuffer1.valid() && frame.gbuffer2.valid();
    });
    if (created)
        return true;
    for (auto& frame : frameBuffers_) {
        for (const auto texture : {frame.rtOutput, frame.normalDepth, frame.gbuffer1, frame.gbuffer2}) {
            if (!texture.valid())
                continue;
            try {
                device_->destroyTextureEx(texture);
            } catch (...) {
            }
        }
        if (frame.oidnBuffer.valid()) {
            try {
                device_->destroyBufferEx(frame.oidnBuffer);
            } catch (...) {
            }
        }
        frame.rtOutput = {};
        frame.oidnBuffer = {};
        frame.normalDepth = {};
        frame.gbuffer1 = {};
        frame.gbuffer2 = {};
        frame.outputExtent = {};
    }
    return false;
}

bool NativeSceneDerivedRuntime::sync(std::span<const NativeSceneDerivedModel> models, Extent3D outputExtent,
                                     std::string* error) {
    if (error != nullptr)
        error->clear();
    if (!ready()) {
        setError(error, "native scene derived runtime is not initialized");
        return false;
    }
    try {
        data_ = makeNativeSceneDerivedData(models);
        if (!ensureTableBufferCapacity(data_, error))
            return false;
        if (!ensureOutputResources(outputExtent, error))
            return false;
        auto& frame = frameBuffers_[device_->currentFrameSlot() % kNativeFramesInFlight];
        uploadVector(*device_, frame.modelToMaterial, data_.modelToMaterial);
        uploadVector(*device_, frame.materialToModel, data_.materialToModel);
        uploadVector(*device_, frame.peekaboo, data_.peekaboo);
        uploadVector(*device_, frame.materialSelected, data_.materialSelected);
        uploadVector(*device_, frame.cloneCount, data_.cloneCount);
        uploadVector(*device_, frame.textureTable, data_.textureTable);
    } catch (const std::exception& exception) {
        setError(error, std::string("native scene derived resource synchronization failed: ") + exception.what());
        return false;
    } catch (...) {
        setError(error, "native scene derived resource synchronization failed");
        return false;
    }
    return true;
}

void NativeSceneDerivedRuntime::apply(NativeSceneResourceBindings& bindings) const noexcept {
    if (!ready())
        return;
    const auto& frame = frameBuffers_[device_->currentFrameSlot() % kNativeFramesInFlight];
    bindings.modelToMaterial = frame.modelToMaterial;
    bindings.materialToModel = frame.materialToModel;
    bindings.peekaboo = frame.peekaboo;
    bindings.materialSelected = frame.materialSelected;
    bindings.cloneCount = frame.cloneCount;
    bindings.textureTable = frame.textureTable;
    bindings.textures = textures_;
    bindings.rtOutput = frame.rtOutput;
    bindings.oidnBuffer = frame.oidnBuffer;
    bindings.normalDepth = frame.normalDepth;
    bindings.gbuffer1 = frame.gbuffer1;
    bindings.gbuffer2 = frame.gbuffer2;
    bindings.gbuffer = frame.gbuffer1;
}

void NativeSceneDerivedRuntime::destroyOwnedResources() noexcept {
    if (device_ == nullptr)
        return;
    try {
        device_->waitIdle();
    } catch (...) {
    }
    for (const auto texture : ownedTextures_) {
        try {
            device_->destroyTextureEx(texture);
        } catch (...) {
        }
    }
    if (fallbackTexture_.valid()) {
        try {
            device_->destroyTextureEx(fallbackTexture_);
        } catch (...) {
        }
    }
    for (auto& frame : frameBuffers_) {
        for (const auto texture : {frame.rtOutput, frame.normalDepth, frame.gbuffer1, frame.gbuffer2}) {
            if (!texture.valid())
                continue;
            try {
                device_->destroyTextureEx(texture);
            } catch (...) {
            }
        }
        if (frame.oidnBuffer.valid()) {
            try {
                device_->destroyBufferEx(frame.oidnBuffer);
            } catch (...) {
            }
        }
        const std::array buffers{frame.modelToMaterial,  frame.materialToModel, frame.peekaboo,
                                 frame.materialSelected, frame.cloneCount,      frame.textureTable};
        for (const auto buffer : buffers) {
            if (!buffer.valid())
                continue;
            try {
                device_->destroyBufferEx(buffer);
            } catch (...) {
            }
        }
        frame = {};
    }
}

void NativeSceneDerivedRuntime::reset() noexcept {
    destroyOwnedResources();
    device_ = nullptr;
    fallbackTexture_ = {};
    textures_.clear();
    ownedTextures_.clear();
    frameBuffers_ = {};
    data_ = {};
}

} // namespace dayo::graphics
