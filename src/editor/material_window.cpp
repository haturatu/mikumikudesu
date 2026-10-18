#include "editor/material_window.hpp"

#include "core/log.hpp"
#include "editor/editor_session.hpp"

namespace dayo::editor {

void MaterialWindow::queueMaterialEdit(EditorSession& session) {
    if (entries_.empty() || session.scene() == nullptr)
        return;
    // Material editing requires a material-specific undoable command. Never
    // masquerade as a motion edit: that pollutes undo history without changing
    // any material state.
    dayo::log::warn("MaterialWindow: material commit is not implemented; no operation was queued");
}

} // namespace dayo::editor
