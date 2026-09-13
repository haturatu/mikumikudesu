#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace dayo::fx {

enum class FxShaderStage : std::uint8_t {
    vertex,
    fragment,
    compute,
    rayGeneration,
    miss,
    closestHit,
    anyHit,
    intersection,
    callable,
};

struct FxShaderCompileRequest {
    std::string hlsl;
    std::filesystem::path sourcePath;
    std::string entryPoint{"main"};
    FxShaderStage stage{FxShaderStage::compute};
    std::vector<std::string> macros;
    std::vector<std::filesystem::path> includeDirectories;
    std::string targetEnvironment{"vulkan1.3"};
};

struct FxShaderArtifact {
    std::vector<std::uint32_t> spirv;
    std::string compiler;
    std::string compilerVersion;
};

// Small, process-isolated HLSL -> SPIR-V compiler adapter. The adapter keeps
// compiler execution outside the Vulkan device so it can be used by hot
// reload workers and by non-Vulkan validation tools alike.
class FxShaderCompiler {
  public:
    explicit FxShaderCompiler(std::filesystem::path executable = {});

    [[nodiscard]] const std::filesystem::path& executable() const noexcept {
        return executable_;
    }
    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] FxShaderArtifact compile(const FxShaderCompileRequest& request) const;

    [[nodiscard]] static std::string_view profile(FxShaderStage stage) noexcept;
    [[nodiscard]] static std::string_view glslcStage(FxShaderStage stage) noexcept;

  private:
    std::filesystem::path executable_;
};

} // namespace dayo::fx
