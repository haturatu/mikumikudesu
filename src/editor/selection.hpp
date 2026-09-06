#pragma once

#include "editor/motion_key_id.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace dayo::editor {

// Index-free selection. UI windows read this model and emit
// EditorOperations; they never index MotionDocument directly.
class Selection {
  public:
    void set(std::vector<MotionKeyId> ids);
    void add(MotionKeyId id);
    bool remove(MotionKeyId id) noexcept;
    void clear() noexcept;
    [[nodiscard]] bool contains(MotionKeyId id) const noexcept;
    [[nodiscard]] const std::vector<MotionKeyId>& ids() const noexcept {
        return ids_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return ids_.empty();
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return ids_.size();
    }
    // Resolve to transient indices for one frame only. Callers must not
    // persist the returned indices across edits.
    [[nodiscard]] std::vector<core::MotionKeyRef> resolveTransient(
        const core::MotionDocument& document, const StableIdTable& table) const;

  private:
    std::vector<MotionKeyId> ids_;
};

} // namespace dayo::editor
