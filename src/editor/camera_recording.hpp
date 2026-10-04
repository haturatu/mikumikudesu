#pragma once
#include "editor/editor_operation.hpp"
namespace dayo::editor {
// One recording owns a fixed overwrite range and one undo transaction.
// Playback updates do not insert keys until the user requests one.
class CameraRecording {
  public:
    CameraRecording(core::Scene& scene, core::CommandHistory& history, StableIdTable& ids, std::int64_t start,
                    std::int64_t end);
    void requestKey() noexcept {
        requested_ = true;
    }
    [[nodiscard]] bool keyRequested() const noexcept {
        return requested_;
    }
    bool update(core::VmdCameraKey key);
    std::vector<MotionKeyId> finish();
    [[nodiscard]] std::int64_t end() const noexcept {
        return end_;
    }

  private:
    core::Scene& scene_;
    StableIdTable& ids_;
    UndoTransaction transaction_;
    std::int64_t start_, end_;
    bool requested_{};
    std::vector<MotionKeyId> recorded_;
};
} // namespace dayo::editor
