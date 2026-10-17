#include "ui/i18n.hpp"

#include "core/log.hpp"

#include <SDL3/SDL.h>

#include <memory>

namespace dayo::ui {
namespace {
std::filesystem::path preferencePath() {
    const std::unique_ptr<char, decltype(&SDL_free)> directory(SDL_GetPrefPath("haturatu", "mikumikudesu"), SDL_free);
    if (directory == nullptr)
        return {};
    const auto path = std::filesystem::path(directory.get()) / "language.txt";
    return path;
}
} // namespace

void initializeEditorLanguage() {
    auto fallback = Language::english;
    const std::unique_ptr<SDL_Locale*, decltype(&SDL_free)> locales(SDL_GetPreferredLocales(nullptr), SDL_free);
    if (locales != nullptr) {
        if (locales.get()[0] != nullptr && locales.get()[0]->language != nullptr) {
            const auto& preferred = *locales.get()[0];
            fallback = languageFromLocale(preferred.language, preferred.country != nullptr ? preferred.country : "");
        }
    }
    loadLanguagePreference(preferencePath(), fallback);
}
bool persistEditorLanguage() {
    const auto path = preferencePath();
    if (path.empty() || !saveLanguagePreference(path)) {
        log::warn("Could not save editor language preference: ", path.string());
        return false;
    }
    return true;
}
} // namespace dayo::ui
