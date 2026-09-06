#include "editor/external_parent_window.hpp"

#include "editor/editor_session.hpp"

namespace dayo::editor {

bool ExternalParentWindow::validate(EditorSession& session) {
    auto* scene = session.scene();
    if (scene == nullptr)
        return false;
    draft_.error.clear();
    draft_.cycle = false;
    if (draft_.parentBone.empty() || draft_.childBone.empty()) {
        draft_.error = "parent and child bones are required";
        return false;
    }
    // Skeleton check: cycle detection requires committed Scene links, so we
    // report the local draft as valid and let the operation layer surface
    // Scene::addExternalParent errors without mutating here.
    if (scene->hasExternalParentCycle()) {
        draft_.cycle = true;
        draft_.error = "external parent cycle detected";
        return false;
    }
    return true;
}

} // namespace dayo::editor
