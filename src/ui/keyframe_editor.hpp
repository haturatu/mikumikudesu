#pragma once

#include "core/editor.hpp"
#include "ui/keyframe_edit.hpp"

#include <imgui.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace dayo::ui {

// A cached view of one model/global motion. Scene owns the document; every
// completed edit goes through CommandHistory. Repaint never copies the motion.
class KeyframeEditor {
  public:
    struct Result {
        std::optional<float> seek;
        bool changed{};
        bool pause{};
    };
    void synchronize(core::Scene& scene, core::ModelId target, bool global);
    Result draw(core::Scene& scene, core::CommandHistory& history, float frame);
    bool drawInspector(core::Scene& scene, core::CommandHistory& history);
    void clearSelection();
    [[nodiscard]] const std::vector<core::MotionKeyRef>& selection() const noexcept {
        return selection_;
    }
    // Read-only geometry also lets interaction tests exercise actual hit targets.
    [[nodiscard]] ImVec2 canvasOrigin() const noexcept {
        return canvasOrigin_;
    }
    [[nodiscard]] float pixelsPerFrame() const noexcept {
        return pixelsPerFrame_;
    }
    [[nodiscard]] float rowHeight() const noexcept {
        return rowHeight_;
    }

  private:
    struct Entry {
        core::MotionKeyRef ref;
        std::uint32_t frame{};
    };
    struct Row {
        core::MotionTrack track{};
        std::string name;
        std::string alias;
        std::vector<Entry> keys;
    };
    struct VisibleKey {
        Entry key;
        ImVec2 position;
    };
    void rebuild(const core::Scene& scene);
    bool matches(const Row& row) const;
    void select(Entry key, const Row& row, bool toggle, bool range);
    void execute(core::Scene& scene, core::CommandHistory& history, core::VmdMotion after, const char* label);
    bool moveSelection(core::Scene& scene, core::CommandHistory& history, std::int64_t delta);
    void drawCanvas(core::Scene& scene, core::CommandHistory& history, float frame, Result& result);
    void drawList(float frame, Result& result);
    void finishGesture(core::Scene& scene, core::CommandHistory& history, Result& result);

    core::ModelId target_{};
    bool global_{};
    std::uint64_t revision_{~std::uint64_t{}};
    std::uint64_t ownRevision_{~std::uint64_t{}};
    const core::VmdMotion* motion_{};
    std::vector<Row> rows_;
    std::vector<core::MotionKeyRef> selection_;
    std::unordered_set<core::MotionKeyRef, MotionKeyRefHash> selectedSet_;
    std::optional<Entry> anchor_;
    core::MotionClipboard clipboard_;
    std::array<char, 128> filter_{};
    int trackFilter_{};
    bool listVisible_{};
    float zoom_{1.0F};
    float startFrame_{};
    float pixelsPerFrame_{1.0F};
    float rowHeight_{24.0F};
    ImVec2 canvasOrigin_{};
    std::vector<VisibleKey> visibleKeys_;
    bool keyGesture_{};
    bool boxGesture_{};
    ImVec2 gestureStart_{};
    std::int64_t dragDelta_{};
    std::optional<core::VmdMotion> dragBefore_;
    std::optional<core::VmdMotion> draft_;
    std::optional<core::MotionKeyRef> draftKey_;
    std::array<char, 1024> exportPath_{};
    std::string status_;
};

} // namespace dayo::ui
