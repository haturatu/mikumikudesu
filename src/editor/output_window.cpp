#include "editor/output_window.hpp"

namespace dayo::editor {

std::string OutputWindow::previewPath(std::uint32_t frame) const {
    return core::outputPath(settings_, frame).string();
}

} // namespace dayo::editor
