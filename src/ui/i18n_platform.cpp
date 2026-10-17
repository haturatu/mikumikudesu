#include "ui/i18n.hpp"

#include "core/log.hpp"

#include <SDL3/SDL.h>

namespace dayo::ui {
namespace {
std::filesystem::path preferencePath() {
    char* directory = SDL_GetPrefPath("haturatu", "mikumikudesu");
    if (directory == nullptr)
        return {};
    const auto path = std::filesystem::path(directory) / "language.txt";
    SDL_free(directory);
    return path;
}
} // namespace

void initializeEditorLanguage() {
    auto fallback = Language::english;
    SDL_Locale** locales = SDL_GetPreferredLocales(nullptr);
    if (locales != nullptr) {
        if (locales[0] != nullptr && locales[0]->language != nullptr)
            fallback = languageFromLocale(locales[0]->language);
        SDL_free(locales);
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
