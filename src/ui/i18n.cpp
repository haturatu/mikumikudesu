#include "ui/i18n.hpp"

#include <fstream>
#include <string>
#include <unordered_map>

namespace dayo::ui {
namespace {
struct Entry {
    const char* source;
    std::array<const char*, 4> translations;
};
constexpr Entry catalog[]{
#include "ui/i18n_catalog.inc"
};
Language currentLanguage = Language::english;
std::size_t index(Language value) noexcept {
    return static_cast<std::size_t>(value);
}
const auto& translations() {
    static const auto table = [] {
        std::unordered_map<std::string_view, const Entry*> result;
        for (const auto& entry : catalog)
            result.emplace(entry.source, &entry);
        return result;
    }();
    return table;
}
} // namespace

Language language() noexcept {
    return currentLanguage;
}
void setLanguage(Language value) noexcept {
    if (index(value) < languages.size())
        currentLanguage = value;
}
Language languageFromLocale(std::string_view locale) noexcept {
    const auto prefix = locale.substr(0, 2);
    if (prefix == "ja" || prefix == "JA")
        return Language::japanese;
    if (prefix == "zh" || prefix == "ZH")
        return Language::chineseSimplified;
    if (prefix == "ru" || prefix == "RU")
        return Language::russian;
    return Language::english;
}
const char* languageCode(Language value) noexcept {
    constexpr std::array codes{"en-US", "ja-JP", "zh-CN", "ru-RU"};
    return codes[index(value) < codes.size() ? index(value) : 0];
}
const char* languageName(Language value) noexcept {
    constexpr std::array names{"English", "日本語", "简体中文", "Русский"};
    return names[index(value) < names.size() ? index(value) : 0];
}
const char* tr(const char* source) {
    const auto found = translations().find(source);
    if (found == translations().end())
        return source;
    return found->second->translations[index(currentLanguage)];
}
const char* label(const char* source) {
    if (std::string_view(source).starts_with("##"))
        return source;
    static std::array<std::unordered_map<std::string, std::string>, 4> cache;
    auto& labels = cache[index(currentLanguage)];
    const auto found = labels.find(source);
    if (found != labels.end())
        return found->second.c_str();
    const std::string key(source);
    const auto separator = key.find("##");
    const std::string visible = key.substr(0, separator);
    auto text = std::string(tr(visible.c_str())) + "###" + key;
    return labels.emplace(key, std::move(text)).first->second.c_str();
}
const char* items(const char* source) {
    static std::array<std::unordered_map<std::string, std::string>, 4> cache;
    auto& lists = cache[index(currentLanguage)];
    const auto found = lists.find(source);
    if (found != lists.end())
        return found->second.c_str();
    std::string result;
    std::string_view remaining(source);
    while (!remaining.empty()) {
        const auto separator = remaining.find('|');
        const std::string key(remaining.substr(0, separator));
        result += tr(key.c_str());
        result.push_back('\0');
        if (separator == std::string_view::npos)
            break;
        remaining.remove_prefix(separator + 1);
    }
    result.push_back('\0');
    return lists.emplace(source, std::move(result)).first->second.c_str();
}
Message::Message(std::string source, std::initializer_list<std::string> arguments)
    : source_(std::move(source)), arguments_(arguments) {}
Message::Message(const char* source) : source_(source) {}
bool Message::empty() const noexcept {
    return source_.empty();
}
std::string Message::render(const char* pattern) const {
    std::string result;
    std::string_view remaining(pattern);
    // Parse placeholders once; never interpret braces inside filenames or diagnostic arguments.
    while (!remaining.empty()) {
        if (remaining.size() >= 3 && remaining[0] == '{' && remaining[1] >= '0' && remaining[1] <= '9' &&
            remaining[2] == '}') {
            const auto argument = static_cast<std::size_t>(remaining[1] - '0');
            if (argument < arguments_.size()) {
                result += arguments_[argument];
                remaining.remove_prefix(3);
                continue;
            }
        }
        result.push_back(remaining.front());
        remaining.remove_prefix(1);
    }
    return result;
}
std::string Message::text() const {
    return render(tr(source_.c_str()));
}
std::ostream& operator<<(std::ostream& output, const Message& message) {
    return output << message.render(message.source_.c_str());
}
void loadLanguagePreference(const std::filesystem::path& path, Language fallback) {
    setLanguage(fallback);
    std::ifstream input(path);
    std::string code;
    if (!(input >> code))
        return;
    for (const auto candidate : languages)
        if (code == languageCode(candidate)) {
            setLanguage(candidate);
            return;
        }
}
bool saveLanguagePreference(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::trunc);
    output << languageCode(currentLanguage) << '\n';
    output.close();
    return !output.fail();
}
} // namespace dayo::ui
