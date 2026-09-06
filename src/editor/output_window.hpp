#pragma once

#include "core/output.hpp"

#include <string>

namespace dayo::editor {

class EditorSession;

// Sequence output panel. Owns OutputSettings as scratch state and formats the
// destination via outputPath(); actual encoding stays in OutputQueue (bounded).
class OutputWindow {
  public:
    void setSettings(core::OutputSettings settings) {
        settings_ = std::move(settings);
    }
    [[nodiscard]] const core::OutputSettings& settings() const noexcept {
        return settings_;
    }
    [[nodiscard]] std::string previewPath(std::uint32_t frame) const;
    [[nodiscard]] const char* titleKey() const noexcept {
        return "window.output";
    }

  private:
    core::OutputSettings settings_;
};

} // namespace dayo::editor
