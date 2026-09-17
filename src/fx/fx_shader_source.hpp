#pragma once

#include "fx/fx_compiler.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace dayo::fx {

enum class FxNativeShaderRegister : std::uint8_t {
    uav,
    sampled,
    sampler,
    uniform,
};

// A renderer-owned declaration that is visible to generated native FX
// shaders. The declaration contains the HLSL resource type and identifier,
// but not the register annotation; the generator appends the ABI-owned
// register and descriptor-set coordinates.
struct FxNativeShaderResource {
    std::string declaration;
    FxNativeShaderRegister registerClass{FxNativeShaderRegister::sampled};
    std::uint32_t registerIndex{};
    std::uint32_t descriptorSet{};
};

struct FxNativeShaderSourceOptions {
    // Optional renderer-owned HLSL type declarations, such as a native
    // material structure. They are emitted before resource declarations.
    std::string preamble;
    std::vector<FxNativeShaderResource> resources;
};

// Builds the source consumed by the native shader compiler. The original FX
// format stores controller and resource declarations next to the pass graph;
// keeping their generated HLSL here makes the compiler input lossless without
// requiring the Vulkan backend to parse effect source text again.
[[nodiscard]] std::string makeNativeFxShaderSource(const FxProgram& program, const FxDispatch& dispatch,
                                                   std::uint32_t resourceSet,
                                                   const FxNativeShaderSourceOptions& options = {});

} // namespace dayo::fx
