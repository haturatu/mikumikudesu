#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace dayo::editor {

class EditorSession;

// Material parameter inspector. Operates on a scratch copy; commit path is
// left as an operation so Scene mutation stays behind CommandHistory.
class MaterialWindow {
  public:
    struct Entry {
        std::string name;
        float value{};
    };
    void setEntries(std::vector<Entry> entries) {
        entries_ = std::move(entries);
    }
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept {
        return entries_;
    }
    void setMaterial(std::size_t material) noexcept {
        material_ = material;
    }
    void queueMaterialEdit(EditorSession& session);
    [[nodiscard]] const char* titleKey() const noexcept {
        return "window.material";
    }

  private:
    std::size_t material_{};
    std::vector<Entry> entries_;
};

} // namespace dayo::editor
