#pragma once

#include <string>
#include <vector>

namespace dayo::editor {

class EditorSession;

// External-parent link editor. Validation lives in Scene::addExternalParent;
// this window only collects (parentBone, childBone) pairs and reports errors.
class ExternalParentWindow {
  public:
    struct Draft {
        std::string parentBone;
        std::string childBone;
        std::string error;
        bool cycle{};
    };
    void setDraft(Draft draft) {
        draft_ = std::move(draft);
    }
    [[nodiscard]] const Draft& draft() const noexcept {
        return draft_;
    }
    bool validate(EditorSession& session);
    [[nodiscard]] const char* titleKey() const noexcept {
        return "window.external_parent";
    }

  private:
    Draft draft_;
};

} // namespace dayo::editor
