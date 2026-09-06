#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dayo::editor {

class EditorSession;
class Localization;

// Keyframe list/detail window. Reads selection + stable ids, writes only via
// EditorOperationQueue (begin/drag/commit coalescing for scrubs).
class KeyframeWindow {
  public:
    struct Row {
        std::uint64_t stableId{};
        std::int32_t track{};
        std::string name;
        std::uint32_t frame{};
        bool selected{};
    };
    void refresh(EditorSession& session);
    // Enqueues a frame move for the current selection; the session coalesces
    // the drag into one undo entry on commit.
    void requestMoveSelected(EditorSession& session, std::int64_t frameDelta);
    [[nodiscard]] const std::vector<Row>& rows() const noexcept {
        return rows_;
    }
    [[nodiscard]] const char* titleKey() const noexcept {
        return "window.keyframe";
    }

  private:
    std::vector<Row> rows_;
};

} // namespace dayo::editor
