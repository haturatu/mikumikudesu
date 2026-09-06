#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace dayo::editor {

// Skeleton string table (en/ja). UI windows resolve labels through this
// instead of hard-coding literals, keeping future .po/json wiring additive.
class Localization {
  public:
    explicit Localization(std::string language = "en") : language_(std::move(language)) {}
    void setLanguage(std::string language) {
        language_ = std::move(language);
    }
    [[nodiscard]] const std::string& language() const noexcept {
        return language_;
    }
    [[nodiscard]] std::string_view get(std::string_view key) const noexcept;

  private:
    std::string language_;
    static const std::unordered_map<std::string, std::string>& english();
    static const std::unordered_map<std::string, std::string>& japanese();
};

} // namespace dayo::editor
