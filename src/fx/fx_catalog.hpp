#pragma once

#include "core/fx/fx_pass.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace dayo::fx {

enum class FxCatalogGroup : std::uint8_t {
    rendererDirectory,
    postprocessDirectory,
    particleDirectory,
    sampleDirectory,
};

[[nodiscard]] const char* toString(FxCatalogGroup group) noexcept;
[[nodiscard]] FxCatalogGroup fxCatalogGroupFromString(std::string_view name) noexcept;

struct FxCatalogEntry {
    std::string name;
    FxCatalogGroup group{FxCatalogGroup::rendererDirectory};
    // This is the execution category from the loaded graph. It is deliberately
    // separate from the directory group because upstream effects may be
    // organized independently of their runtime category.
    core::fx::FxCategory executionCategory{core::fx::FxCategory::render};
    std::filesystem::path path;
    bool controllerEnabled{true};
    int executionOrder{};
};

// Backend-neutral effect catalog. Scans the upstream-style layout:
//   <root>/renderer/*.fxdayo, <root>/postprocess/**/*.fxdayo,
//   <root>/particle/*.fxdayo, <root>/sample/*.fxdayo
// Individual scan*() entry points keep renderer/post/particle/sample
// callers independent; scanAll() is the union.
class EffectCatalog {
  public:
    void add(FxCatalogEntry entry);
    void scanRenderer(const std::filesystem::path& directory);
    void scanPostProcess(const std::filesystem::path& directory);
    void scanParticle(const std::filesystem::path& directory);
    void scanSample(const std::filesystem::path& directory);
    void scanAll(const std::filesystem::path& root);
    [[nodiscard]] std::vector<FxCatalogEntry> find(FxCatalogGroup group) const;
    [[nodiscard]] const std::vector<FxCatalogEntry>& all() const noexcept {
        return entries_;
    }
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }

  private:
    void scanDirectory(const std::filesystem::path& directory, FxCatalogGroup group, bool recursive);

    std::vector<FxCatalogEntry> entries_;
};

} // namespace dayo::fx
