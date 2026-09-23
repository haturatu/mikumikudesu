#include "graphics/fx_material_gpu_runtime.hpp"

#include "core/image.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace dayo::graphics {
namespace {

[[nodiscard]] std::string upper(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value)
        result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
    return result;
}

[[nodiscard]] std::uint32_t fullMipCount(Extent3D extent) noexcept {
    auto largest = std::max({extent.width, extent.height, extent.depth});
    std::uint32_t levels = 1;
    while (largest > 1) {
        largest /= 2;
        ++levels;
    }
    return levels;
}

[[nodiscard]] std::vector<std::byte> rawBytes(std::span<const std::uint32_t> values, std::uint32_t emptyValue) {
    if (!values.empty()) {
        const auto bytes = std::as_bytes(values);
        return {bytes.begin(), bytes.end()};
    }
    std::vector<std::byte> bytes(sizeof(emptyValue));
    std::memcpy(bytes.data(), &emptyValue, sizeof(emptyValue));
    return bytes;
}

[[nodiscard]] std::vector<std::uint32_t> descriptorTextureIndices(std::span<const std::uint32_t> indices) {
    std::vector<std::uint32_t> result;
    result.reserve(indices.size());
    for (const auto index : indices) {
        if (index == core::fx::kMissingMaterialTextureIndex) {
            result.push_back(index);
            continue;
        }
        if (index == core::fx::kMissingMaterialTextureIndex - 1U)
            throw std::overflow_error("MatDesc physical texture index collides with the fallback slot");
        result.push_back(index + 1U);
    }
    return result;
}

[[nodiscard]] std::vector<std::byte> valueBytes(const core::fx::MaterialStructuredBufferData& values) {
    if (!values.bytes.empty())
        return values.bytes;
    const auto emptySize = std::max<std::size_t>(values.layout.stride, sizeof(std::uint32_t));
    return std::vector<std::byte>(emptySize, std::byte{0});
}

[[nodiscard]] std::size_t checkedBufferSize(std::size_t actual, std::string_view name) {
    if (actual > std::numeric_limits<std::size_t>::max() - 3U)
        throw std::overflow_error("MatDesc " + std::string(name) + " buffer size overflow");
    return std::max<std::size_t>((actual + 3U) & ~std::size_t{3U}, sizeof(std::uint32_t));
}

[[nodiscard]] std::size_t checkedIndexBytes(std::size_t count, std::string_view name) {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t))
        throw std::overflow_error("MatDesc " + std::string(name) + " buffer size overflow");
    return checkedBufferSize(count * sizeof(std::uint32_t), name);
}

[[nodiscard]] std::size_t checkedProduct(std::size_t left, std::size_t right, std::string_view name) {
    if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right)
        throw std::overflow_error("MatDesc " + std::string(name) + " size overflow");
    return left * right;
}

void validateTable(const core::fx::MaterialGpuTableData& table) {
    const auto expectedValueBytes = checkedProduct(table.values.count, table.values.layout.stride, "_value");
    if (table.values.layout.stride == 0 || table.values.bytes.size() != expectedValueBytes)
        throw std::invalid_argument("MatDesc value bytes do not match the structured-buffer layout");

    const auto expectedTextureEntries = checkedProduct(table.values.count, table.textureSlotCount, "texture table");
    if (table.textureIndices2D.size() != expectedTextureEntries ||
        table.textureIndices3D.size() != expectedTextureEntries)
        throw std::invalid_argument("MatDesc texture index arrays do not match value rows and logical slots");
    if (!std::ranges::all_of(table.materialIndices,
                             [&table](std::uint32_t index) { return index <= table.values.count; }))
        throw std::invalid_argument("MatDesc model material base index exceeds the value table");

    const auto validIndices = [](std::span<const std::uint32_t> indices, std::size_t textureCount) {
        return std::ranges::all_of(indices, [textureCount](std::uint32_t index) {
            return index == core::fx::kMissingMaterialTextureIndex || index < textureCount;
        });
    };
    if (!validIndices(table.textureIndices2D, table.textures2D.size()) ||
        !validIndices(table.textureIndices3D, table.textures3D.size()))
        throw std::invalid_argument("MatDesc texture index references a missing physical texture");
    constexpr auto maxCatalogSize = static_cast<std::size_t>(core::fx::kMissingMaterialTextureIndex - 1U);
    if (table.textures2D.size() > maxCatalogSize || table.textures3D.size() > maxCatalogSize)
        throw std::overflow_error("MatDesc physical texture catalog leaves no index for its fallback slot");
    if (!std::ranges::all_of(
            table.textures2D,
            [](const auto& texture) { return texture.dimension == core::fx::MaterialTextureDimension::twoD; }) ||
        !std::ranges::all_of(table.textures3D, [](const auto& texture) {
            return texture.dimension == core::fx::MaterialTextureDimension::threeD;
        }))
        throw std::invalid_argument("MatDesc texture catalog contains an entry with the wrong dimension");
}

[[nodiscard]] TextureResourceDesc sampledTextureDesc(TextureDimension dimension, Extent3D extent,
                                                     std::uint32_t mipLevels, PixelFormat format) {
    return {.dimension = dimension,
            .extent = extent,
            .format = format,
            .mipLevels = mipLevels,
            .arrayLayers = 1,
            .usage = ResourceUsage::sampledRead | ResourceUsage::transferDst |
                     (mipLevels > 1 ? ResourceUsage::transferSrc : ResourceUsage::none),
            .lifetime = ResourceLifetime::persistent};
}

[[nodiscard]] handles::TextureHandle createFallbackTexture(Device& device, TextureDimension dimension) {
    const auto handle = device.createTextureEx(sampledTextureDesc(dimension, {1, 1, 1}, 1, PixelFormat::rgba8Unorm));
    if (!handle.valid())
        throw std::runtime_error("MatDesc fallback texture allocation returned an invalid handle");
    constexpr std::array<std::uint8_t, 4> white{255, 255, 255, 255};
    try {
        device.uploadTextureEx(handle, white, 0, 0);
    } catch (...) {
        device.destroyTextureEx(handle);
        throw;
    }
    return handle;
}

[[nodiscard]] handles::TextureHandle loadMaterialTexture(Device& device, const core::fx::MaterialTextureDesc& texture,
                                                         core::fx::MaterialTextureDimension dimension) {
    const std::filesystem::path path(texture.path);
    if (path.empty())
        throw std::invalid_argument("MatDesc physical texture path is empty");
    const auto format = upper(texture.format);
    if (!format.empty() && format != "RGBA8_UNORM" && format != "R8G8B8A8_UNORM" && format != "RGBA8_SRGB" &&
        format != "R8G8B8A8_SRGB")
        throw std::invalid_argument("MatDesc texture format is unsupported: " + texture.format);
    const auto colorspace = upper(texture.colorspace);
    const auto pixelFormat = format.ends_with("_SRGB") || colorspace == "SRGB" || colorspace == "SRGB_TEXTURE"
                                 ? PixelFormat::rgba8Srgb
                                 : PixelFormat::rgba8Unorm;
    const auto extension = upper(path.extension().string());
    if (extension == ".DDS") {
        const auto dds = core::loadDdsImageRgba8(path);
        const bool is3D = dimension == core::fx::MaterialTextureDimension::threeD;
        if (dds.arrayLayers != 1 ||
            (is3D ? dds.dimension != core::DdsDimension::threeD : dds.dimension != core::DdsDimension::twoD))
            throw std::invalid_argument("MatDesc DDS dimensionality does not match its texture declaration: " +
                                        path.string());
        const Extent3D extent{dds.width, dds.height, dds.depth};
        const auto mipLevels = !texture.mipmapped ? 1U : dds.mipLevels > 1 ? dds.mipLevels : fullMipCount(extent);
        const auto handle = device.createTextureEx(
            sampledTextureDesc(is3D ? TextureDimension::d3 : TextureDimension::d2, extent, mipLevels, pixelFormat));
        if (!handle.valid())
            throw std::runtime_error("MatDesc DDS texture allocation returned an invalid handle: " + path.string());
        try {
            const auto uploadedLevels = std::min(mipLevels, dds.mipLevels);
            for (std::uint32_t mip = 0; mip < uploadedLevels; ++mip)
                device.uploadTextureEx(handle, dds.subresource(mip).pixels, mip, 0);
            if (mipLevels > uploadedLevels)
                device.generateMipmapsEx(handle);
        } catch (...) {
            device.destroyTextureEx(handle);
            throw;
        }
        return handle;
    }
    if (dimension == core::fx::MaterialTextureDimension::threeD)
        throw std::invalid_argument("MatDesc 3D textures must be DDS volumes: " + path.string());
    const auto image = core::loadImageRgba8(path);
    const Extent3D extent{image.width, image.height, 1};
    const auto mipLevels = texture.mipmapped ? fullMipCount(extent) : 1U;
    const auto handle =
        device.createTextureEx(sampledTextureDesc(TextureDimension::d2, extent, mipLevels, pixelFormat));
    if (!handle.valid())
        throw std::runtime_error("MatDesc texture allocation returned an invalid handle: " + path.string());
    try {
        device.uploadTextureEx(handle, image.pixels, 0, 0);
        if (mipLevels > 1)
            device.generateMipmapsEx(handle);
    } catch (...) {
        device.destroyTextureEx(handle);
        throw;
    }
    return handle;
}

[[nodiscard]] std::vector<std::string> textureCatalogKeys(std::span<const core::fx::MaterialTextureDesc> textures) {
    std::vector<std::string> keys;
    keys.reserve(textures.size());
    for (const auto& texture : textures)
        keys.push_back(core::fx::textureKeyString(core::fx::makeTextureKey(texture)));
    return keys;
}

} // namespace

FxMaterialGpuRuntime::~FxMaterialGpuRuntime() {
    reset();
}

bool FxMaterialGpuRuntime::sync(Device& device, const core::fx::MaterialGpuTableData& table, std::string* error) {
    if (error != nullptr)
        error->clear();
    try {
        validateTable(table);
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = exception.what();
        return false;
    }
    if (device_ != nullptr && device_ != &device) {
        if (error != nullptr)
            *error = "MatDesc GPU resources belong to a different device";
        return false;
    }
    device_ = &device;
    try {
        bool buffersRecreated = false;
        if (!ensureFallbackTextures(error) ||
            !ensureTextureCatalog(table.textures2D, core::fx::MaterialTextureDimension::twoD, error) ||
            !ensureTextureCatalog(table.textures3D, core::fx::MaterialTextureDimension::threeD, error) ||
            !ensureTableBuffers(table, buffersRecreated, error))
            throw std::runtime_error(error != nullptr && !error->empty() ? *error : "MatDesc GPU sync failed");
        if (buffersRecreated) {
            for (std::size_t slot = 0; slot < kNativeFramesInFlight; ++slot)
                if (!uploadFrame(table, slot, error))
                    throw std::runtime_error(error != nullptr && !error->empty() ? *error
                                                                                 : "MatDesc table upload failed");
        } else {
            const auto slot = device.currentFrameSlot() % kNativeFramesInFlight;
            if (!uploadFrame(table, slot, error))
                throw std::runtime_error(error != nullptr && !error->empty() ? *error : "MatDesc table upload failed");
        }
    } catch (const std::exception& exception) {
        if (error != nullptr && error->empty())
            *error = exception.what();
        reset();
        return false;
    } catch (...) {
        if (error != nullptr && error->empty())
            *error = "MatDesc GPU resource synchronization failed";
        reset();
        return false;
    }
    return true;
}

bool FxMaterialGpuRuntime::ensureFallbackTextures(std::string* error) {
    try {
        if (!fallbackTexture2D_.valid())
            fallbackTexture2D_ = createFallbackTexture(*device_, TextureDimension::d2);
        if (!fallbackTexture3D_.valid())
            fallbackTexture3D_ = createFallbackTexture(*device_, TextureDimension::d3);
        if (textureBindings2D_.empty())
            textureBindings2D_.push_back(fallbackTexture2D_);
        if (textureBindings3D_.empty())
            textureBindings3D_.push_back(fallbackTexture3D_);
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = exception.what();
        return false;
    }
    return true;
}

bool FxMaterialGpuRuntime::ensureTextureCatalog(std::span<const core::fx::MaterialTextureDesc> textures,
                                                core::fx::MaterialTextureDimension dimension, std::string* error) {
    auto& currentTextures = dimension == core::fx::MaterialTextureDimension::twoD ? textures2D_ : textures3D_;
    auto& currentBindings =
        dimension == core::fx::MaterialTextureDimension::twoD ? textureBindings2D_ : textureBindings3D_;
    auto& currentKeys = dimension == core::fx::MaterialTextureDimension::twoD ? textureKeys2D_ : textureKeys3D_;
    const auto requestedKeys = textureCatalogKeys(textures);
    if (requestedKeys == currentKeys)
        return true;

    std::vector<handles::TextureHandle> created;
    std::vector<handles::TextureHandle> createdBindings;
    try {
        created.reserve(textures.size());
        for (const auto& texture : textures)
            created.push_back(loadMaterialTexture(*device_, texture, dimension));
        createdBindings.reserve(created.size() + 1U);
        createdBindings.push_back(dimension == core::fx::MaterialTextureDimension::twoD ? fallbackTexture2D_
                                                                                        : fallbackTexture3D_);
        createdBindings.insert(createdBindings.end(), created.begin(), created.end());
    } catch (const std::exception& exception) {
        destroyTextures(created);
        if (error != nullptr)
            *error = exception.what();
        return false;
    } catch (...) {
        destroyTextures(created);
        if (error != nullptr)
            *error = "MatDesc texture catalog creation failed";
        return false;
    }
    try {
        device_->waitIdle();
    } catch (...) {
    }
    destroyTextures(currentTextures);
    currentTextures = std::move(created);
    currentBindings = std::move(createdBindings);
    currentKeys = requestedKeys;
    return true;
}

bool FxMaterialGpuRuntime::ensureTableBuffers(const core::fx::MaterialGpuTableData& table, bool& recreated,
                                              std::string* error) {
    recreated = false;
    if (table.values.layout.stride == 0) {
        if (error != nullptr)
            *error = "MatDesc value layout has a zero stride";
        return false;
    }
    const std::array required{
        checkedIndexBytes(table.materialIndices.size(), "_idx"),
        checkedIndexBytes(table.textureIndices2D.size(), "_tex"),
        checkedIndexBytes(table.textureIndices3D.size(), "_tex3D"),
        checkedBufferSize(std::max(table.values.bytes.size(), table.values.layout.stride), "_value")};
    const bool alreadySized = std::ranges::all_of(frameBuffers_, [&required](const auto& frame) {
        return frame.capacity == required && frame.materialIndices.valid() && frame.textureIndices2D.valid() &&
               frame.textureIndices3D.valid() && frame.values.valid();
    });
    if (alreadySized)
        return true;

    std::array<FrameBuffers, kNativeFramesInFlight> created{};
    try {
        for (auto& frame : created) {
            const auto makeBuffer = [&device = *device_](std::size_t size) {
                const auto handle =
                    device.createBufferEx({.size = size,
                                           .usage = ResourceUsage::storageRead | ResourceUsage::hostRead,
                                           .cpuVisible = true,
                                           .lifetime = ResourceLifetime::persistent});
                if (!handle.valid())
                    throw std::runtime_error("MatDesc GPU buffer allocation returned an invalid handle");
                return handle;
            };
            frame.materialIndices = makeBuffer(required[0]);
            frame.textureIndices2D = makeBuffer(required[1]);
            frame.textureIndices3D = makeBuffer(required[2]);
            frame.values = makeBuffer(required[3]);
            frame.capacity = required;
        }
    } catch (const std::exception& exception) {
        destroyBuffers(created);
        if (error != nullptr)
            *error = exception.what();
        return false;
    }
    try {
        device_->waitIdle();
    } catch (...) {
    }
    destroyBuffers(frameBuffers_);
    frameBuffers_ = created;
    recreated = true;
    return true;
}

bool FxMaterialGpuRuntime::uploadFrame(const core::fx::MaterialGpuTableData& table, std::size_t frameSlot,
                                       std::string* error) {
    auto& frame = frameBuffers_[frameSlot % kNativeFramesInFlight];
    const auto indexBytes = rawBytes(table.materialIndices, 0U);
    const auto texture2DIndices = descriptorTextureIndices(table.textureIndices2D);
    const auto texture3DIndices = descriptorTextureIndices(table.textureIndices3D);
    const auto texture2DBytes = rawBytes(texture2DIndices, core::fx::kMissingMaterialTextureIndex);
    const auto texture3DBytes = rawBytes(texture3DIndices, core::fx::kMissingMaterialTextureIndex);
    const auto valueData = valueBytes(table.values);
    try {
        device_->uploadBufferEx(frame.materialIndices, indexBytes, 0);
        device_->uploadBufferEx(frame.textureIndices2D, texture2DBytes, 0);
        device_->uploadBufferEx(frame.textureIndices3D, texture3DBytes, 0);
        device_->uploadBufferEx(frame.values, valueData, 0);
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = exception.what();
        return false;
    }
    return true;
}

void FxMaterialGpuRuntime::destroyBuffers(std::span<FrameBuffers> buffers) noexcept {
    if (device_ == nullptr)
        return;
    for (auto& frame : buffers) {
        const std::array handles{frame.materialIndices, frame.textureIndices2D, frame.textureIndices3D, frame.values};
        for (const auto handle : handles) {
            if (!handle.valid())
                continue;
            try {
                device_->destroyBufferEx(handle);
            } catch (...) {
            }
        }
        frame = {};
    }
}

void FxMaterialGpuRuntime::destroyTextures(std::span<handles::TextureHandle> textures) noexcept {
    if (device_ == nullptr)
        return;
    for (const auto texture : textures) {
        if (!texture.valid())
            continue;
        try {
            device_->destroyTextureEx(texture);
        } catch (...) {
        }
    }
}

void FxMaterialGpuRuntime::reset() noexcept {
    Device* device = device_;
    if (device != nullptr) {
        try {
            device->waitIdle();
        } catch (...) {
        }
    }
    destroyBuffers(frameBuffers_);
    destroyTextures(textures2D_);
    destroyTextures(textures3D_);
    textures2D_.clear();
    textures3D_.clear();
    textureBindings2D_.clear();
    textureBindings3D_.clear();
    if (device != nullptr) {
        for (const auto texture : {fallbackTexture2D_, fallbackTexture3D_}) {
            if (!texture.valid())
                continue;
            try {
                device->destroyTextureEx(texture);
            } catch (...) {
            }
        }
    }
    fallbackTexture2D_ = {};
    fallbackTexture3D_ = {};
    textureKeys2D_.clear();
    textureKeys3D_.clear();
    device_ = nullptr;
}

bool FxMaterialGpuRuntime::ready() const noexcept {
    return device_ != nullptr && fallbackTexture2D_.valid() && fallbackTexture3D_.valid() &&
           std::ranges::all_of(frameBuffers_,
                               [](const auto& frame) {
                                   return frame.materialIndices.valid() && frame.textureIndices2D.valid() &&
                                          frame.textureIndices3D.valid() && frame.values.valid();
                               }) &&
           std::ranges::all_of(textures2D_, [](const auto texture) { return texture.valid(); }) &&
           std::ranges::all_of(textures3D_, [](const auto texture) { return texture.valid(); });
}

FxMaterialGpuBindings FxMaterialGpuRuntime::bindings() const noexcept {
    return bindings(device_ == nullptr ? 0U : device_->currentFrameSlot());
}

FxMaterialGpuBindings FxMaterialGpuRuntime::bindings(std::size_t frameSlot) const noexcept {
    const auto slot = frameSlot % kNativeFramesInFlight;
    const auto& frame = frameBuffers_[slot];
    return {.materialIndices = frame.materialIndices,
            .textureIndices2D = frame.textureIndices2D,
            .textureIndices3D = frame.textureIndices3D,
            .values = frame.values,
            .textures2D = textures2D(),
            .textures3D = textures3D()};
}

std::span<const handles::TextureHandle> FxMaterialGpuRuntime::textures2D() const noexcept {
    return textureBindings2D_;
}

std::span<const handles::TextureHandle> FxMaterialGpuRuntime::textures3D() const noexcept {
    return textureBindings3D_;
}

} // namespace dayo::graphics
