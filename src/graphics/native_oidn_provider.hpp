#pragma once

#include "core/denoiser.hpp"
#include "fx/fx_compiler.hpp"
#include "graphics/fx_executor.hpp"

#include <optional>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dayo::graphics {

// Host adapter for the upstream OIDN operation. The generic executor owns
// pass ordering; this class only translates typed textures to the existing
// bounded CPU/GPU denoiser runtime and writes the result back to the declared
// output texture.
class NativeOidnProvider {
  public:
    NativeOidnProvider() = default;
    explicit NativeOidnProvider(Device& device) noexcept : device_(&device) {}

    void setDevice(Device* device) noexcept {
        device_ = device;
    }

    [[nodiscard]] bool execute(const fx::FxOidnDispatch& dispatch, const fx::FxFrameContext& context,
                               CommandList& commands, const FxExecutionResources::TypedResourceResolver& resolve,
                               std::string* error = nullptr);

    [[nodiscard]] core::DenoiserRuntime& runtime() noexcept {
        return denoiser_;
    }

  private:
    [[nodiscard]] static std::vector<float> decodeRgb(std::span<const std::uint8_t> bytes,
                                                       std::uint32_t width, std::uint32_t height);
    [[nodiscard]] static std::vector<std::uint8_t> encodeRgba16(std::span<const float> rgb,
                                                                std::uint32_t width, std::uint32_t height);
    [[nodiscard]] static float halfToFloat(std::uint16_t value) noexcept;
    [[nodiscard]] static std::uint16_t floatToHalf(float value) noexcept;
    static void setError(std::string* error, std::string message);

    Device* device_{};
    core::DenoiserRuntime denoiser_;
};

} // namespace dayo::graphics
