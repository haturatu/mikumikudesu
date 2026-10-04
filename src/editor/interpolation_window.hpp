#pragma once

#include "editor/motion_key_id.hpp"
#include <array>
#include <cstdint>
#include <optional>

namespace dayo::editor {
class EditorSession;
struct CurveEditState {
    std::array<std::uint8_t, 4> points{20, 20, 107, 107};
    std::uint8_t method{};
    friend bool operator==(const CurveEditState&, const CurveEditState&) = default;
};
class InterpolationWindow {
  public:
    void setTarget(std::uint64_t stableId, std::size_t axis) noexcept {
        target_ = {core::MotionTrack::bone, stableId};
        axis_ = axis;
    }
    void setMethod(std::uint8_t method) noexcept {
        curve_.method = method == 1 ? 1 : 0;
    }
    void setCurve(CurveEditState curve) noexcept;
    [[nodiscard]] const CurveEditState& curve() const noexcept {
        return curve_;
    }
    [[nodiscard]] float evaluate(float t) const noexcept;
    [[nodiscard]] std::optional<CurveEditState> selectedCurve(EditorSession& session, std::size_t axis) const;
    std::size_t commit(EditorSession& session, std::size_t axis, bool allAxes = false);
    void commitMethod(EditorSession& session);
    [[nodiscard]] const char* titleKey() const noexcept {
        return "window.interpolation";
    }

  private:
    MotionKeyId target_;
    std::size_t axis_{};
    CurveEditState curve_;
};
} // namespace dayo::editor
