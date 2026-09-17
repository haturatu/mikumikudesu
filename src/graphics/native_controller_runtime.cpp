#include "graphics/native_controller_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace dayo::graphics {
namespace {

void setError(std::string* error, std::string value) {
    if (error != nullptr)
        *error = std::move(value);
}

[[nodiscard]] std::size_t align16(std::size_t value) {
    constexpr std::size_t alignment = 16;
    const auto remainder = value % alignment;
    if (remainder == 0)
        return value;
    const auto delta = alignment - remainder;
    if (value > std::numeric_limits<std::size_t>::max() - delta)
        throw std::overflow_error("native controller cbuffer size overflow");
    return value + delta;
}

[[nodiscard]] std::size_t checkedAdd(std::size_t left, std::size_t right) {
    if (left > std::numeric_limits<std::size_t>::max() - right)
        throw std::overflow_error("native controller cbuffer size overflow");
    return left + right;
}

[[nodiscard]] std::size_t checkedMultiply(std::size_t left, std::size_t right) {
    if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right)
        throw std::overflow_error("native controller array size overflow");
    return left * right;
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

struct ParsedName {
    std::string name;
    std::uint32_t arrayCount{1};
};

[[nodiscard]] ParsedName parseName(std::string_view source) {
    const auto bracket = source.find('[');
    if (bracket == std::string_view::npos)
        return {identifier(source), 1};
    if (source.empty() || source.back() != ']' || bracket == 0)
        throw std::invalid_argument("native controller has an invalid array name: " + std::string(source));
    const auto countText = source.substr(bracket + 1, source.size() - bracket - 2);
    if (countText.empty())
        throw std::invalid_argument("native controller has an empty array size: " + std::string(source));
    std::uint64_t count = 0;
    for (const auto character : countText) {
        if (!std::isdigit(static_cast<unsigned char>(character)))
            throw std::invalid_argument("native controller has an invalid array size: " + std::string(source));
        count = count * 10U + static_cast<unsigned>(character - '0');
        if (count > std::numeric_limits<std::uint32_t>::max())
            throw std::overflow_error("native controller array size overflow: " + std::string(source));
    }
    if (count == 0)
        throw std::invalid_argument("native controller array size must be non-zero: " + std::string(source));
    return {identifier(source.substr(0, bracket)), static_cast<std::uint32_t>(count)};
}

[[nodiscard]] NativeControllerType parseType(std::string_view source) {
    if (source.empty() || source == "float")
        return NativeControllerType::scalar;
    if (source == "bool")
        return NativeControllerType::boolean;
    if (source == "int")
        return NativeControllerType::integer;
    if (source == "uint")
        return NativeControllerType::unsignedInteger;
    if (source == "float2")
        return NativeControllerType::vector2;
    if (source == "float3")
        return NativeControllerType::vector3;
    if (source == "float4")
        return NativeControllerType::vector4;
    if (source == "float4x4")
        return NativeControllerType::matrix4x4;
    throw std::invalid_argument("native controller type is unsupported: " + std::string(source));
}

[[nodiscard]] std::size_t typeSize(NativeControllerType type) noexcept {
    switch (type) {
    case NativeControllerType::boolean:
    case NativeControllerType::integer:
    case NativeControllerType::unsignedInteger:
    case NativeControllerType::scalar:
        return 4;
    case NativeControllerType::vector2:
        return 8;
    case NativeControllerType::vector3:
        return 12;
    case NativeControllerType::vector4:
        return 16;
    case NativeControllerType::matrix4x4:
        return 64;
    }
    return 0;
}

[[nodiscard]] bool isMatrix(NativeControllerType type) noexcept {
    return type == NativeControllerType::matrix4x4;
}

[[nodiscard]] bool isCompatible(const NativeControllerField& field, NativeControllerType type) noexcept {
    return field.type == type;
}

template <typename T> bool writeValue(std::byte* destination, const T& value) noexcept {
    if (destination == nullptr)
        return false;
    std::memcpy(destination, &value, sizeof(value));
    return true;
}

template <std::size_t N> bool writeArray(std::byte* destination, const std::array<float, N>& value) noexcept {
    if (destination == nullptr)
        return false;
    std::memcpy(destination, value.data(), sizeof(value));
    return true;
}

} // namespace

const NativeControllerField* NativeControllerLayout::find(std::string_view name) const noexcept {
    const auto found = std::find_if(fields.begin(), fields.end(), [name](const auto& field) {
        return field.name == name || field.name == identifier(name);
    });
    return found == fields.end() ? nullptr : &*found;
}

NativeControllerLayout makeNativeControllerLayout(std::span<const core::EffectController> controllers) {
    NativeControllerLayout result;
    std::size_t cursor = 0;
    for (const auto& controller : controllers) {
        const auto parsed = parseName(controller.name);
        if (parsed.name.empty())
            throw std::invalid_argument("native controller name is empty");
        if (result.find(parsed.name) != nullptr)
            throw std::invalid_argument("native controller name is duplicated: " + parsed.name);
        const auto type = parseType(controller.type);
        const auto elementSize = typeSize(type);
        if (elementSize == 0)
            throw std::invalid_argument("native controller type has no storage size: " + parsed.name);

        NativeControllerField field{.name = parsed.name,
                                    .type = type,
                                    .arrayCount = parsed.arrayCount,
                                    .offset = 0,
                                    .elementSize = elementSize,
                                    .elementStride = elementSize};
        if (parsed.arrayCount > 1U || isMatrix(type)) {
            cursor = align16(cursor);
            field.offset = cursor;
            field.elementStride = isMatrix(type) ? elementSize : 16;
            cursor = checkedAdd(cursor, checkedMultiply(field.elementStride, parsed.arrayCount));
        } else {
            if (elementSize > 16U || cursor % 16U + elementSize > 16U)
                cursor = align16(cursor);
            field.offset = cursor;
            cursor = checkedAdd(cursor, elementSize);
        }
        if (cursor > kMaxNativeControllerBytes)
            throw std::length_error("native controller cbuffer exceeds the 64 KiB limit");
        result.fields.push_back(std::move(field));
    }
    result.byteSize = std::max<std::size_t>(16, align16(cursor));
    return result;
}

NativeControllerBlock::NativeControllerBlock(NativeControllerLayout layout)
    : layout_(std::move(layout)), bytes_(layout_.byteSize, std::byte{0}) {}

std::byte* NativeControllerBlock::element(const NativeControllerField& field, std::size_t arrayIndex) noexcept {
    if (arrayIndex >= field.arrayCount ||
        (field.elementStride != 0 &&
         arrayIndex > (std::numeric_limits<std::size_t>::max() - field.offset) / field.elementStride))
        return nullptr;
    const auto offset = field.offset + field.elementStride * arrayIndex;
    if (offset > bytes_.size() || field.elementSize > bytes_.size() - offset)
        return nullptr;
    return bytes_.data() + offset;
}

bool NativeControllerBlock::setBool(std::string_view name, bool value, std::size_t arrayIndex) noexcept {
    const auto* field = layout_.find(name);
    if (field == nullptr || !isCompatible(*field, NativeControllerType::boolean))
        return false;
    const auto encoded = static_cast<std::int32_t>(value ? 1 : 0);
    return writeValue(element(*field, arrayIndex), encoded);
}

bool NativeControllerBlock::setInt(std::string_view name, std::int32_t value, std::size_t arrayIndex) noexcept {
    const auto* field = layout_.find(name);
    if (field == nullptr || !isCompatible(*field, NativeControllerType::integer))
        return false;
    return writeValue(element(*field, arrayIndex), value);
}

bool NativeControllerBlock::setUInt(std::string_view name, std::uint32_t value, std::size_t arrayIndex) noexcept {
    const auto* field = layout_.find(name);
    if (field == nullptr || !isCompatible(*field, NativeControllerType::unsignedInteger))
        return false;
    return writeValue(element(*field, arrayIndex), value);
}

bool NativeControllerBlock::setFloat(std::string_view name, float value, std::size_t arrayIndex) noexcept {
    const auto* field = layout_.find(name);
    if (field == nullptr || !isCompatible(*field, NativeControllerType::scalar))
        return false;
    return writeValue(element(*field, arrayIndex), value);
}

bool NativeControllerBlock::setFloat2(std::string_view name, const std::array<float, 2>& value,
                                      std::size_t arrayIndex) noexcept {
    const auto* field = layout_.find(name);
    if (field == nullptr || !isCompatible(*field, NativeControllerType::vector2))
        return false;
    return writeArray(element(*field, arrayIndex), value);
}

bool NativeControllerBlock::setFloat3(std::string_view name, const std::array<float, 3>& value,
                                      std::size_t arrayIndex) noexcept {
    const auto* field = layout_.find(name);
    if (field == nullptr || !isCompatible(*field, NativeControllerType::vector3))
        return false;
    return writeArray(element(*field, arrayIndex), value);
}

bool NativeControllerBlock::setFloat4(std::string_view name, const std::array<float, 4>& value,
                                      std::size_t arrayIndex) noexcept {
    const auto* field = layout_.find(name);
    if (field == nullptr || !isCompatible(*field, NativeControllerType::vector4))
        return false;
    return writeArray(element(*field, arrayIndex), value);
}

bool NativeControllerBlock::setMatrix4x4(std::string_view name, const std::array<float, 16>& value,
                                         std::size_t arrayIndex) noexcept {
    const auto* field = layout_.find(name);
    if (field == nullptr || !isCompatible(*field, NativeControllerType::matrix4x4))
        return false;
    return writeArray(element(*field, arrayIndex), value);
}

NativeControllerRuntime::~NativeControllerRuntime() {
    reset();
}

bool NativeControllerRuntime::initialize(Device& device, std::span<const core::EffectController> controllers,
                                         std::string* error) {
    if (error != nullptr)
        error->clear();
    reset();
    device_ = &device;
    try {
        layout_ = makeNativeControllerLayout(controllers);
        for (auto& buffer : buffers_) {
            buffer = device.createBufferEx({.size = layout_.byteSize,
                                            .usage = ResourceUsage::uniformRead | ResourceUsage::hostRead,
                                            .cpuVisible = true,
                                            .lifetime = ResourceLifetime::persistent});
            if (!buffer.valid())
                throw std::runtime_error("native controller buffer is invalid");
        }
        const std::vector<std::byte> initial(layout_.byteSize, std::byte{0});
        for (const auto buffer : buffers_)
            device.uploadBufferEx(buffer, initial, 0);
    } catch (const std::exception& exception) {
        setError(error, std::string("native controller buffer initialization failed: ") + exception.what());
        reset();
        return false;
    } catch (...) {
        setError(error, "native controller buffer initialization failed");
        reset();
        return false;
    }
    return true;
}

bool NativeControllerRuntime::sync(Device& device, std::span<const std::byte> bytes, std::string* error) {
    if (error != nullptr)
        error->clear();
    if (device_ == nullptr || !ready()) {
        setError(error, "native controller buffer is not initialized");
        return false;
    }
    if (device_ != &device) {
        setError(error, "native controller buffer belongs to a different device");
        return false;
    }
    if (bytes.size() != layout_.byteSize) {
        setError(error, "native controller data size does not match its cbuffer layout");
        return false;
    }
    try {
        const auto buffer = buffers_[device_->currentFrameSlot() % kNativeFramesInFlight];
        device_->uploadBufferEx(buffer, bytes, 0);
    } catch (const std::exception& exception) {
        setError(error, std::string("native controller buffer upload failed: ") + exception.what());
        return false;
    } catch (...) {
        setError(error, "native controller buffer upload failed");
        return false;
    }
    return true;
}

void NativeControllerRuntime::reset() noexcept {
    Device* device = device_;
    if (device != nullptr) {
        try {
            device->waitIdle();
        } catch (...) {
        }
        for (const auto buffer : buffers_) {
            if (buffer.valid()) {
                try {
                    device->destroyBufferEx(buffer);
                } catch (...) {
                }
            }
        }
    }
    device_ = nullptr;
    layout_ = {};
    buffers_.fill({});
}

} // namespace dayo::graphics
