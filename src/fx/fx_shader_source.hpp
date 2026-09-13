#pragma once

#include "fx/fx_compiler.hpp"

#include <cstdint>
#include <string>

namespace dayo::fx {

// Builds the source consumed by the native shader compiler. The original FX
// format stores controller and resource declarations next to the pass graph;
// keeping their generated HLSL here makes the compiler input lossless without
// requiring the Vulkan backend to parse effect source text again.
[[nodiscard]] std::string makeNativeFxShaderSource(const FxProgram& program, const FxDispatch& dispatch,
                                                   std::uint32_t resourceSet);

} // namespace dayo::fx
