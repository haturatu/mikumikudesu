#pragma once

#include "core/effect.hpp"
#include "graphics/device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dayo::graphics {

inline constexpr std::size_t kMaxNativeControllerBytes = 64U * 1024U;

// HLSL constant-buffer types supported by the generated YRZFX controller
// block. The enum is deliberately independent from MaterialValue: controller
// values are packed according to cbuffer register rules, not C++ object layout.
enum class NativeControllerType : std::uint8_t {
    boolean,
    integer,
    unsignedInteger,
    scalar,
    vector2,
    vector3,
    vector4,
    matrix4x4,
};

struct NativeControllerField {
    std::string name;
    NativeControllerType type{NativeControllerType::scalar};
    std::uint32_t arrayCount{1};
    std::size_t offset{};
    std::size_t elementSize{};
    std::size_t elementStride{};
};

// Describes the exact byte layout of YRZFX_ControllerCB. Scalar/vector
// members share a 16-byte register when possible; arrays and matrices start
// each element on a fresh register, matching HLSL cbuffer packing.
struct NativeControllerLayout {
    std::vector<NativeControllerField> fields;
    std::size_t byteSize{16};

    [[nodiscard]] const NativeControllerField* find(std::string_view name) const noexcept;
};

[[nodiscard]] NativeControllerLayout
makeNativeControllerLayout(std::span<const core::EffectController> controllers);

// Mutable host-side values for one generated controller block. The block is
// initialized to zero so an effect remains deterministic until its application
// controller values are supplied. Setters reject type/array mismatches.
class NativeControllerBlock {
  public:
    explicit NativeControllerBlock(NativeControllerLayout layout);

    [[nodiscard]] const NativeControllerLayout& layout() const noexcept {
        return layout_;
    }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return bytes_;
    }

    bool setBool(std::string_view name, bool value, std::size_t arrayIndex = 0) noexcept;
    bool setInt(std::string_view name, std::int32_t value, std::size_t arrayIndex = 0) noexcept;
    bool setUInt(std::string_view name, std::uint32_t value, std::size_t arrayIndex = 0) noexcept;
    bool setFloat(std::string_view name, float value, std::size_t arrayIndex = 0) noexcept;
    bool setFloat2(std::string_view name, const std::array<float, 2>& value,
                   std::size_t arrayIndex = 0) noexcept;
    bool setFloat3(std::string_view name, const std::array<float, 3>& value,
                   std::size_t arrayIndex = 0) noexcept;
    bool setFloat4(std::string_view name, const std::array<float, 4>& value,
                   std::size_t arrayIndex = 0) noexcept;
    bool setMatrix4x4(std::string_view name, const std::array<float, 16>& value,
                     std::size_t arrayIndex = 0) noexcept;

  private:
    [[nodiscard]] std::byte* element(const NativeControllerField& field,
                                      std::size_t arrayIndex) noexcept;

    NativeControllerLayout layout_;
    std::vector<std::byte> bytes_;
};

// Owns the device-local uniform buffer bound at register b1 / native binding
// 49. The runtime intentionally accepts a byte span so future MMD controller
// sources can populate the same ABI without changing the GPU contract.
class NativeControllerRuntime {
  public:
    NativeControllerRuntime() = default;
    ~NativeControllerRuntime();

    NativeControllerRuntime(const NativeControllerRuntime&) = delete;
    NativeControllerRuntime& operator=(const NativeControllerRuntime&) = delete;

    [[nodiscard]] bool initialize(Device& device, std::span<const core::EffectController> controllers,
                                  std::string* error = nullptr);
    [[nodiscard]] bool sync(Device& device, std::span<const std::byte> bytes,
                            std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr && buffer_.valid();
    }
    [[nodiscard]] const NativeControllerLayout& layout() const noexcept {
        return layout_;
    }
    [[nodiscard]] handles::BufferHandle buffer() const noexcept {
        return buffer_;
    }

  private:
    Device* device_{};
    NativeControllerLayout layout_;
    handles::BufferHandle buffer_{};
};

} // namespace dayo::graphics
