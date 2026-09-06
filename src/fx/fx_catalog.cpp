#include "fx/fx_catalog.hpp"

#include <filesystem>
#include <system_error>

namespace dayo::fx {

const char* toString(FxCategory category) noexcept {
    switch (category) {
    case FxCategory::renderer:
        return "renderer";
    case FxCategory::postprocess:
        return "postprocess";
    case FxCategory::particle:
        return "particle";
    case FxCategory::sample:
        return "sample";
    case FxCategory::unknown:
        return "unknown";
    }
    return "unknown";
}

FxCategory fxCategoryFromString(std::string_view name) noexcept {
    if (name == "renderer")
        return FxCategory::renderer;
    if (name == "postprocess")
        return FxCategory::postprocess;
    if (name == "particle")
        return FxCategory::particle;
    if (name == "sample")
        return FxCategory::sample;
    return FxCategory::unknown;
}

void EffectCatalog::add(FxCatalogEntry entry) {
    entries_.push_back(std::move(entry));
}

void EffectCatalog::scanDirectory(const std::filesystem::path& directory, FxCategory category, bool recursive) {
    std::error_code error;
    if (!std::filesystem::exists(directory, error))
        return;
    const auto pushEntry = [&](const std::filesystem::directory_entry& file) {
        if (!file.is_regular_file())
            return;
        if (file.path().extension() != ".fxdayo")
            return;
        FxCatalogEntry entry;
        entry.name = file.path().stem().string();
        entry.category = category;
        entry.path = file.path();
        entries_.push_back(std::move(entry));
    };
    if (recursive) {
        for (std::filesystem::recursive_directory_iterator it(directory, error), end; it != end; it.increment(error)) {
            if (error)
                break;
            pushEntry(*it);
        }
    } else {
        for (std::filesystem::directory_iterator it(directory, error), end; it != end; it.increment(error)) {
            if (error)
                break;
            pushEntry(*it);
        }
    }
}

void EffectCatalog::scanRenderer(const std::filesystem::path& directory) {
    scanDirectory(directory, FxCategory::renderer, false);
}

void EffectCatalog::scanPostProcess(const std::filesystem::path& directory) {
    scanDirectory(directory, FxCategory::postprocess, true);
}

void EffectCatalog::scanParticle(const std::filesystem::path& directory) {
    scanDirectory(directory, FxCategory::particle, false);
}

void EffectCatalog::scanSample(const std::filesystem::path& directory) {
    scanDirectory(directory, FxCategory::sample, false);
}

void EffectCatalog::scanAll(const std::filesystem::path& root) {
    scanRenderer(root / "renderer");
    scanPostProcess(root / "postprocess");
    scanParticle(root / "particle");
    scanSample(root / "sample");
}

std::vector<FxCatalogEntry> EffectCatalog::find(FxCategory category) const {
    std::vector<FxCatalogEntry> result;
    for (const auto& entry : entries_) {
        if (entry.category == category)
            result.push_back(entry);
    }
    return result;
}

void EffectCatalog::clear() noexcept {
    entries_.clear();
}

} // namespace dayo::fx
