#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dayo::editor {

class EditorSession;

// Read-only FX runtime inspector. Consumes FxRuntimeDebugSnapshot data via
// strings/values only; raw handles are never touched here.
class FxDebugWindow {
  public:
    struct SnapshotView {
        std::uint64_t frame{};
        std::uint32_t drawCount{};
        std::uint32_t materialCount{};
        std::string backend;
        std::vector<std::string> passes;
    };
    void setSnapshot(SnapshotView view) {
        view_ = std::move(view);
    }
    [[nodiscard]] const SnapshotView& view() const noexcept {
        return view_;
    }
    [[nodiscard]] std::string summary() const;
    [[nodiscard]] const char* titleKey() const noexcept {
        return "window.fx_debug";
    }

  private:
    SnapshotView view_;
};

} // namespace dayo::editor
