#pragma once

#include "core/motion.hpp"

#include <array>
#include <cstdint>

namespace dayo::editor {

class EditorSession;

// Bezier/catmull-rom inspector for one bone axis. Preview is computed
// locally; commit goes through ReplaceMotionOperation only.
class InterpolationWindow {
  public:
    void setTarget(std::uint64_t stableId, std::size_t axis) noexcept {
        stableId_ = stableId;
        axis_ = axis > 3 ? 3 : axis;
    }
    void setMethod(std::uint8_t method) noexcept {
        method_ = method;
    }
    [[nodiscard]] float evaluate(float t) const noexcept;
    void commitMethod(EditorSession& session);
    [[nodiscard]] const char* titleKey() const noexcept {
        return "window.interpolation";
    }

  private:
    std::uint64_t stableId_{};
    std::size_t axis_{};
    std::uint8_t method_{};
    std::array<std::uint8_t, 4> points_{20, 20, 107, 107};
};

} // namespace dayo::editor
