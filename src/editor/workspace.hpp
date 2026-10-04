#pragma once
#include "editor/editor_session.hpp"
#include "editor/interpolation_window.hpp"
#include "editor/pose_binding.hpp"
#include "editor/viewport_math.hpp"
#include "graphics/camera_matrices.hpp"
namespace dayo::editor {
class Workspace {
  public:
    bool drawModels(EditorSession& session, float frame);
    void drawHistory(EditorSession& session);
    bool drawBonePanel(EditorSession& session, PoseBinding& binding, core::ModelInstance& model, float frame);
    void drawInterpolation(EditorSession& session, InterpolationWindow& window);
    bool drawViewport(PoseBinding& binding, const core::ModelInstance& model,
                      const graphics::SceneCameraMatrices& camera, ScreenRect viewport, bool hovered,
                      bool rigidBodies = false);
    [[nodiscard]] bool manipulating() const noexcept;

  private:
    core::ModelId parentModel_{};
    int parentBone_{};
    int childBone_{};
    std::string parentError_;
    bool world_{};
    bool rotate_{};
    bool boxSelecting_{};
    ScreenPoint boxBegin_{};
    std::vector<int> boxOriginal_;
    int curveAxis_{};
    bool allAxes_{};
    bool curveDirty_{};
    std::optional<CurveEditState> curveClipboard_;
    std::vector<MotionKeyId> curveSelection_;
    std::uint64_t curveRevision_{};
    int loadedAxis_{-1};
};
} // namespace dayo::editor
