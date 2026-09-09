#include "ui/fonts.hpp"

#if DAYO_HAS_IMGUI
#include "core/log.hpp"

#include <imgui.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace dayo::ui {
namespace {

std::vector<std::filesystem::path> fontCandidates() {
    std::vector<std::filesystem::path> result;
    if (const auto* configured = std::getenv("DAYO_FONT_PATH"); configured != nullptr && configured[0] != '\0')
        result.emplace_back(configured);
    result.emplace_back("assets/fonts/NotoSansCJKjp-Regular.otf");
    result.emplace_back("assets/fonts/NotoSansJP-Regular.ttf");
    result.emplace_back("assets/fonts/NotoSansCJK-Regular.ttc");
    result.emplace_back("share/mikumikudesu/fonts/NotoSansCJKjp-Regular.otf");
    result.emplace_back("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
    result.emplace_back("/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc");
    result.emplace_back("C:/Windows/Fonts/NotoSansJP-Regular.otf");
    result.emplace_back("C:/Windows/Fonts/meiryo.ttc");
    return result;
}

} // namespace

void loadEditorFonts() {
    auto& io = ImGui::GetIO();
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 1;
    config.GlyphRanges = io.Fonts->GetGlyphRangesJapanese();
    for (const auto& candidate : fontCandidates()) {
        if (!std::filesystem::exists(candidate))
            continue;
        if (io.Fonts->AddFontFromFileTTF(candidate.string().c_str(), 16.0F, &config) != nullptr) {
            log::info("ImGui Japanese font loaded: ", candidate.string());
            return;
        }
    }
    log::warn("Japanese ImGui font not found; set DAYO_FONT_PATH or install Noto Sans CJK JP");
}

} // namespace dayo::ui
#endif
