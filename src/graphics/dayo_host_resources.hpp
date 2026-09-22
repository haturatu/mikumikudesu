#pragma once

#include "graphics/native_scene_resource_runtime.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace dayo::graphics {

enum class DayoSemantic : std::uint8_t {
    RTOutput,
    OIDNBuf,
    NormalDepth,
    GBuffer1,
    GBuffer2,
    GBuffer,
    TLAS,
    Model2Mat,
    Mat2Model,
    Peekaboo,
    MatSelected,
    Skybox,
    Skywalker,
    SkywalkerRow,
    SkyboxSH,
    ScreenBMP,
    CloneCount,
    ScreenTexture,
    ViewCB,
    ControllerCB,
    TextureTable,
    CBuff1,
};

struct DayoResourceBinding {
    handles::TextureHandle texture{};
    handles::BufferHandle buffer{};
    handles::AccelerationStructureHandle accelerationStructure{};

    [[nodiscard]] bool valid() const noexcept {
        return texture.valid() || buffer.valid() || accelerationStructure.valid();
    }
};

[[nodiscard]] constexpr std::uint64_t dayoSemanticBit(DayoSemantic semantic) noexcept {
    return std::uint64_t{1} << static_cast<std::uint8_t>(semantic);
}

[[nodiscard]] constexpr std::uint64_t allDayoSemanticBits() noexcept {
    return (std::uint64_t{1} << (static_cast<std::uint8_t>(DayoSemantic::CBuff1) + 1U)) - 1U;
}

[[nodiscard]] std::string_view toString(DayoSemantic semantic) noexcept;
[[nodiscard]] std::optional<DayoSemantic> dayoSemanticFromString(std::string_view name) noexcept;

// Resolves the canonical upstream semantic names against one frame's scene
// resource bindings. Presence is checked independently from handle validity
// so store-owned placeholders cannot satisfy a required FX binding.
class DayoHostResourceProvider {
  public:
    explicit DayoHostResourceProvider(const NativeSceneResourceBindings& bindings) noexcept : bindings_(&bindings) {}

    [[nodiscard]] std::optional<DayoResourceBinding> resolve(DayoSemantic semantic) const noexcept;
    [[nodiscard]] std::optional<DayoResourceBinding> resolve(std::string_view semantic) const noexcept;
    [[nodiscard]] bool require(std::span<const DayoSemantic> semantics, std::string* error = nullptr) const;

    [[nodiscard]] const NativeSceneResourceBindings& bindings() const noexcept {
        return *bindings_;
    }

  private:
    const NativeSceneResourceBindings* bindings_{};
};

} // namespace dayo::graphics
