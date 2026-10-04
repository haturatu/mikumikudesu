#pragma once

#include "core/scene.hpp"
#include "editor/editor_operation.hpp"
#include "editor/selection.hpp"

#include <cstdint>

namespace dayo::editor {

struct EditorConfig;
class Localization;

// Session owns UI-side state only. Scene mutation flows exclusively through
// EditorOperationQueue::flush(scene, history).
class EditorSession {
  public:
    EditorSession(core::Scene* scene, core::CommandHistory* history) : scene_(scene), history_(history) {}

    void setTarget(core::ModelId target, bool global);
    void moveKeyframeDrag(std::int64_t delta);
    void beginKeyframeDrag(const std::string& label = "Drag keys");
    void updateKeyframeDrag(core::VmdMotion intermediate);
    void commitKeyframeDrag();
    void cancelKeyframeDrag() noexcept;
    std::size_t flushOperations();

    [[nodiscard]] core::Scene* scene() noexcept {
        return scene_;
    }
    [[nodiscard]] core::CommandHistory* history() noexcept {
        return history_;
    }
    [[nodiscard]] Selection& selection() noexcept {
        return selection_;
    }
    [[nodiscard]] const Selection& selection() const noexcept {
        return selection_;
    }
    [[nodiscard]] EditorOperationQueue& operations() noexcept {
        return operations_;
    }
    [[nodiscard]] StableIdTable& stableIds();
    [[nodiscard]] StableIdTable& stableIds(core::ModelId target, bool global);
    [[nodiscard]] core::ModelId target() const noexcept {
        return target_;
    }
    [[nodiscard]] bool global() const noexcept {
        return global_;
    }
    [[nodiscard]] bool dragging() const noexcept {
        return transaction_ != nullptr;
    }

  private:
    core::Scene* scene_{};
    core::CommandHistory* history_{};
    Selection selection_;
    EditorOperationQueue operations_;
    StableIdTable globalStableIds_;
    std::unordered_map<core::ModelId, std::unique_ptr<StableIdTable>> modelStableIds_;
    std::optional<core::VmdMotion> dragOriginal_;
    StableIdTable dragOriginalIds_;
    std::vector<MotionKeyId> dragKeys_;
    std::int64_t dragDelta_{};
    bool dragChanged_{};
    core::ModelId target_{};
    bool global_{};
    std::unique_ptr<UndoTransaction> transaction_;
};

} // namespace dayo::editor
