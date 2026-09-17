#pragma once

#include "fx/fx_compiler.hpp"
#include "graphics/device.hpp"

#include <string>

namespace dayo::graphics {

// The result of attempting to activate a native renderer for one effect. A
// Preview selection is always considered safe; nativeReady only describes a
// successfully initialized Subayai or BDPT runtime.
struct NativeRendererStatus {
    RendererKind requested{RendererKind::preview};
    RendererKind active{RendererKind::preview};
    bool nativeReady{};
    std::string reason;

    [[nodiscard]] bool fellBack() const noexcept {
        return requested != active;
    }
};

// Effect requirements are checked in addition to the renderer's baseline
// device capability. This prevents a renderer name from accidentally making
// an effect with a stronger RT contract executable.
[[nodiscard]] std::string missingEffectFeatures(const DeviceCapabilities& capabilities,
                                                const fx::FxRequiredFeatures& required);
[[nodiscard]] NativeRendererStatus decideNativeRenderer(const DeviceCapabilities& capabilities, RendererKind requested,
                                                        const fx::FxRequiredFeatures& required);
[[nodiscard]] NativeRendererStatus decideNativeRendererForInitialization(const DeviceCapabilities& capabilities,
                                                                         RendererKind requested,
                                                                         const fx::FxRequiredFeatures& required);

} // namespace dayo::graphics
