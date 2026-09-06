#pragma once

#include "core/editor.hpp"
#include "core/scene.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace dayo::editor {

// UI -> EditorOperation -> CommandHistory -> Scene.
// Windows enqueue operations; only flush() mutates the Scene via history.
struct SetFrameOperation {
    float frame{};
};

struct ReplaceMotionOperation {
    core::ModelId target{};
    bool global{};
    core::VmdMotion motion;
    std::string label{"Edit motion"};
};

struct MoveKeysOperation {
    core::ModelId target{};
    bool global{};
    // Stable ids; resolved transiently at flush time.
    std::vector<std::uint64_t> stableIds;
    std::int32_t track{};
    std::int64_t frameDelta{};
};

using EditorOperation = std::variant<SetFrameOperation, ReplaceMotionOperation, MoveKeysOperation>;

class EditorOperationQueue {
  public:
    void push(EditorOperation operation);
    // Applies every queued operation through history. Returns applied count.
    std::size_t flush(core::Scene& scene, core::CommandHistory& history);
    void discard() noexcept;
    [[nodiscard]] bool empty() const noexcept {
        return operations_.empty();
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return operations_.size();
    }

  private:
    std::vector<EditorOperation> operations_;
};

// Drag transaction: begin captures `before`, dragTo updates the preview
// without pushing history, commit coalesces the whole drag into a single
// EditMotionCommand. Destruction without commit rolls back the preview.
class UndoTransaction {
  public:
    UndoTransaction(core::Scene& scene, core::CommandHistory& history, core::ModelId target, bool global,
                    std::string label = "Drag keys");
    ~UndoTransaction();
    UndoTransaction(const UndoTransaction&) = delete;
    UndoTransaction& operator=(const UndoTransaction&) = delete;

    void dragTo(core::VmdMotion intermediate);
    void commit();
    void rollback() noexcept;
    [[nodiscard]] bool active() const noexcept {
        return active_;
    }

  private:
    core::Scene* scene_{};
    core::CommandHistory* history_{};
    core::ModelId target_{};
    bool global_{};
    std::string label_;
    std::optional<core::VmdMotion> before_;
    std::optional<core::VmdMotion> current_;
    bool active_{true};
};

} // namespace dayo::editor
