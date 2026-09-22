#include "fx/fx_catalog.hpp"

#include "core/effect.hpp"
#include "core/log.hpp"

#include <filesystem>
#include <stdexcept>
#include <system_error>

namespace dayo::fx {

const char* toString(FxCatalogGroup group) noexcept {
    switch (group) {
    case FxCatalogGroup::rendererDirectory:
        return "renderer";
    case FxCatalogGroup::postprocessDirectory:
        return "postprocess";
    case FxCatalogGroup::particleDirectory:
        return "particle";
    case FxCatalogGroup::sampleDirectory:
        return "sample";
    }
    return "renderer";
}

FxCatalogGroup fxCatalogGroupFromString(std::string_view name) noexcept {
    if (name == "renderer")
        return FxCatalogGroup::rendererDirectory;
    if (name == "postprocess")
        return FxCatalogGroup::postprocessDirectory;
    if (name == "particle")
        return FxCatalogGroup::particleDirectory;
    if (name == "sample")
        return FxCatalogGroup::sampleDirectory;
    return FxCatalogGroup::rendererDirectory;
}

void EffectCatalog::add(FxCatalogEntry entry) {
    entries_.push_back(std::move(entry));
}

void EffectCatalog::scanDirectory(const std::filesystem::path& directory, FxCatalogGroup group, bool recursive) {
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
        entry.group = group;
        entry.path = file.path();
        try {
            const auto graph = core::loadEffectGraph(file.path());
            entry.executionCategory = core::fx::fxCategoryFromString(graph.category);
        } catch (const std::exception& exception) {
            // A catalog entry without an upstream category is unsafe to
            // schedule. Keep the scan useful for editors by skipping only the
            // invalid entry and reporting the reason on the warning stream.
            log::warn("fx catalog: skipping ", file.path().string(), ": ", exception.what());
            return;
        }
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
    scanDirectory(directory, FxCatalogGroup::rendererDirectory, false);
}

void EffectCatalog::scanPostProcess(const std::filesystem::path& directory) {
    scanDirectory(directory, FxCatalogGroup::postprocessDirectory, true);
}

void EffectCatalog::scanParticle(const std::filesystem::path& directory) {
    scanDirectory(directory, FxCatalogGroup::particleDirectory, false);
}

void EffectCatalog::scanSample(const std::filesystem::path& directory) {
    scanDirectory(directory, FxCatalogGroup::sampleDirectory, false);
}

void EffectCatalog::scanAll(const std::filesystem::path& root) {
    scanRenderer(root / "renderer");
    scanPostProcess(root / "postprocess");
    scanParticle(root / "particle");
    scanSample(root / "sample");
}

std::vector<FxCatalogEntry> EffectCatalog::find(FxCatalogGroup group) const {
    std::vector<FxCatalogEntry> result;
    for (const auto& entry : entries_) {
        if (entry.group == group)
            result.push_back(entry);
    }
    return result;
}

void EffectCatalog::clear() noexcept {
    entries_.clear();
}

} // namespace dayo::fx
