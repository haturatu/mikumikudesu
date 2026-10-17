#include "ui/i18n.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
void expect(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void checkLanguages() {
    using namespace dayo::ui;
    expect(languageFromLocale("ja_JP.UTF-8") == Language::japanese, "POSIX Japanese locale");
    expect(languageFromLocale("zh-CN") == Language::chineseSimplified, "Chinese locale");
    expect(languageFromLocale("ru_RU.UTF-8") == Language::russian, "Russian locale");
    expect(languageFromLocale("de-DE") == Language::english, "unsupported locales fall back to English");
    std::string stableId;
    for (const auto candidate : languages) {
        setLanguage(candidate);
        const std::string widget(label("Bone##InspectorBoneSelector"));
        const auto separator = widget.find("###");
        expect(separator != std::string::npos, "visible labels need stable ImGui identifiers");
        if (stableId.empty())
            stableId = widget.substr(separator);
        expect(widget.substr(separator) == stableId, "identifiers must not change across languages");
        expect(std::string(label("Bone##LegacyBoneSelector")) != widget, "distinct source IDs must not collide");
        expect(std::string(label("##hidden")) == "##hidden", "hidden widget IDs must remain unchanged");
        const char* choices = items("Camera|Light|Self shadow");
        for (const auto* source : {"Camera", "Light", "Self shadow"}) {
            expect(std::string(choices) == tr(source), "combo options must translate individually");
            choices += std::string_view(choices).size() + 1;
        }
        expect(*choices == '\0', "combo list must end in a second NUL");
        expect(std::string(tr("user supplied model name")) == "user supplied model name", "unknown names preserved");
    }
    setLanguage(Language::russian);
    expect(std::string(tr("Save")) == "Сохранить", "Russian translation");
    setLanguage(Language::japanese);
    expect(std::string(tr("Save")) == "保存", "Japanese translation");
}
void checkChineseLocales() {
    using namespace dayo::ui;
    for (const auto* locale : {"zh-TW", "zh-HK", "zh-MO", "zh-Hant", "zh-Hant-CN", "zh-Hant-SG", "zh_Hant_TW.UTF-8",
                               "ZH_hK.utf8", "zh-Latn-CN", "zh-x-Hans", "zho-CN", "zheng", "zh", "", "C.UTF-8"})
        expect(languageFromLocale(locale) == Language::english, locale);
    for (const auto* locale : {"zh-CN", "zh-SG", "zh-Hans", "zh-Hans-CN", "zh-Hans-SG", "zh-Hans-TW", "zh_CN.UTF-8",
                               "zH_sG.utf8", "ZH-hAnS", "zh-Hans-CN-u-nu-hanidec", "zh-CN-x-Hant"})
        expect(languageFromLocale(locale) == Language::chineseSimplified, locale);
    expect(languageFromLocale("zh", "CN") == Language::chineseSimplified, "SDL reports China separately");
    expect(languageFromLocale("zh", "SG") == Language::chineseSimplified, "SDL reports Singapore separately");
    for (const auto* country : {"TW", "HK", "MO", ""})
        expect(languageFromLocale("zh", country) == Language::english, "SDL traditional/unspecified Chinese locale");
    expect(languageFromLocale("zh-Hant", "CN") == Language::english, "Hant must override SDL country CN");
    expect(languageFromLocale("zh-Hans", "TW") == Language::chineseSimplified, "Hans must override SDL country TW");
    expect(languageFromLocale("zh-TW", "CN") == Language::english, "tag region must override separate country");
    expect(languageFromLocale("zh-Hans", "") == Language::chineseSimplified, "script works without SDL country");
    expect(languageFromLocale("Ja_jp.UTF-8") == Language::japanese, "mixed case Japanese locale");
    expect(languageFromLocale("rU-RU") == Language::russian, "mixed case Russian locale");
    expect(languageFromLocale("jargon") == Language::english, "primary language requires an exact match");
}
void checkNotifications() {
    using namespace dayo::ui;
    const Message status("Project {0} — assets: {1}", {"モデル{1}%.dayo", "3"});
    setLanguage(Language::japanese);
    expect(status.text() == "プロジェクト モデル{1}%.dayo — アセット数：3",
           "notification translation must preserve filename placeholders");
    setLanguage(Language::russian);
    expect(status.text() == "Проект モデル{1}%.dayo — ресурсов: 3",
           "stored notifications must follow live language changes");
    std::ostringstream log;
    log << status;
    expect(log.str() == "Project モデル{1}%.dayo — assets: 3", "logs must retain source language and arguments");
}
void checkPreferences(const std::filesystem::path& path) {
    using namespace dayo::ui;
    for (const auto candidate : languages) {
        setLanguage(candidate);
        expect(saveLanguagePreference(path), "save preference");
        setLanguage(Language::english);
        loadLanguagePreference(path, Language::english);
        expect(language() == candidate, "chosen language must survive restart");
    }
    {
        std::ofstream output(path);
        output << "invalid\n";
    }
    loadLanguagePreference(path, Language::japanese);
    expect(language() == Language::japanese, "invalid preference must use system language");
    std::filesystem::remove(path);
    loadLanguagePreference(path, Language::russian);
    expect(language() == Language::russian, "missing preference must use system language");
    expect(!saveLanguagePreference(path / "missing"), "failed saves must be reported");
}
} // namespace
int main() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() / ("dayo-i18n-" + std::to_string(suffix));
    int result = 0;
    try {
        checkLanguages();
        checkChineseLocales();
        checkNotifications();
        checkPreferences(path);
        std::cout << "Language switching, stable IDs, combo options and preferences passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return result;
}
