#pragma once

namespace dayo::graphics {

struct NativeFxPendingEvents {
    bool modelChanged{};
    bool materialChanged{};

    void latch(bool model, bool material) noexcept {
        modelChanged = modelChanged || model;
        materialChanged = materialChanged || material;
    }

    void clear() noexcept {
        modelChanged = false;
        materialChanged = false;
    }
};

} // namespace dayo::graphics
