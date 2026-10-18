#pragma once

#include <array>
#include <filesystem>
#include <initializer_list>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace dayo::ui {

enum class Language { english, japanese, chineseSimplified, russian };
inline constexpr std::array languages{Language::english, Language::japanese, Language::chineseSimplified,
                                      Language::russian};
Language language() noexcept;
void setLanguage(Language value) noexcept;
// Accept BCP 47 / POSIX tags, plus SDL's separately reported optional country.
Language languageFromLocale(std::string_view locale, std::string_view country = {}) noexcept;
const char* languageCode(Language value) noexcept;
const char* languageName(Language value) noexcept;
// Unknown strings are returned unchanged. The caller retains ownership of those strings.
const char* tr(const char* source);
// Widget labels keep the source key as an ImGui ### identifier across language changes.
const char* label(const char* source);
// Pipe-separated source keys become the double-NUL-terminated list required by ImGui::Combo.
const char* items(const char* source);
void loadLanguagePreference(const std::filesystem::path& path, Language fallback);
bool saveLanguagePreference(const std::filesystem::path& path);
// Retain source keys and arguments so notifications also follow live language changes.
class Message {
  public:
    Message(std::string source, std::initializer_list<std::string> arguments = {});
    Message(const char* source);
    bool empty() const noexcept;
    std::string text() const;
    friend std::ostream& operator<<(std::ostream& output, const Message& message);

  private:
    std::string source_;
    std::vector<std::string> arguments_;
    std::string render(const char* pattern) const;
};
// SDL platform integration, called once when initializing the editor.
void initializeEditorLanguage();
bool persistEditorLanguage();

} // namespace dayo::ui
