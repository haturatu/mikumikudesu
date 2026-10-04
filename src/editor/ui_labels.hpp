#pragma once
#include <string_view>
namespace dayo::editor {
void setUiLanguage(std::string_view language);
[[nodiscard]] const char* uiLabel(const char* english);
} // namespace dayo::editor
