#include "editor/fx_debug_window.hpp"

#include <sstream>

namespace dayo::editor {

std::string FxDebugWindow::summary() const {
    std::ostringstream line;
    line << "frame=" << view_.frame << " draws=" << view_.drawCount << " materials=" << view_.materialCount
         << " backend=" << view_.backend;
    return line.str();
}

} // namespace dayo::editor
