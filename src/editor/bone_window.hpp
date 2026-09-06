#pragma once

#include "core/model_probe.hpp"

#include <string>

namespace dayo::editor {

class EditorSession;

// Bone pose editor. Edits a scratch copy and commits via ReplaceMotion;
// never writes Scene/model state directly (Vercel: minimal, single focus).
class BoneWindow {
  public:
    void setBone(std::string name) {
        bone_ = std::move(name);
    }
    void setTranslation(core::Float3 value) noexcept {
        translation_ = value;
    }
    void setRotation(core::Float4 value) noexcept {
        rotation_ = value;
    }
    void commitAsNewKey(EditorSession& session, std::uint32_t frame);
    [[nodiscard]] const std::string& bone() const noexcept {
        return bone_;
    }
    [[nodiscard]] const char* titleKey() const noexcept {
        return "window.bone";
    }

  private:
    std::string bone_;
    core::Float3 translation_{};
    core::Float4 rotation_{0.0F, 0.0F, 0.0F, 1.0F};
};

} // namespace dayo::editor
