#include "editor/material_window.hpp"
#include "editor/editor_session.hpp"
#include <cmath>
namespace dayo::editor {
void MaterialWindow::queueMaterialEdit(EditorSession& session) {
    auto* model = session.scene() ? session.scene()->model(session.target()) : nullptr;
    if (!model || material_ >= model->materialSettings.size() || entries_.empty())
        return;
    auto state = model->materialSettings[material_];
    for (const auto& entry : entries_)
        if (!entry.name.empty() && std::isfinite(entry.value))
            state.parameters.set(entry.name, entry.value);
    session.operations().push(
        MaterialEditOperation{model->id, material_, std::move(state), "Edit material parameters"});
}
} // namespace dayo::editor
