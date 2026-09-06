#pragma once

#include "fx/fx_compiler.hpp"
#include "fx/fx_frame.hpp"

#include <array>
#include <string>
#include <vector>

namespace dayo::fx {

// Preview 5-pass reference layout (existing Preview path stays untouched;
// this is the plan -> executor -> backend delegation skeleton):
//   BG (clear) -> MMD (raster) -> GBuffer (raster) -> Copy (copy) -> DENOISE (compute)
enum class PreviewFxPass : std::uint8_t {
    bg,
    mmd,
    gbuffer,
    copy,
    denoise,
};

[[nodiscard]] const char* toString(PreviewFxPass pass) noexcept;
[[nodiscard]] constexpr std::size_t previewFxPassCount() noexcept {
    return 5;
}

[[nodiscard]] FxProgram makePreviewReferenceProgram();
[[nodiscard]] FxFramePlan buildPreviewReferencePlan(const FxFrameContext& context);
[[nodiscard]] FxFramePlan buildPreviewReferencePlan(const FxProgram& program, const FxFrameContext& context);

} // namespace dayo::fx
