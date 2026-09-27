#pragma once
#include "fx/fx_compiler.hpp"
#include <unordered_map>

namespace dayo::fx {
[[nodiscard]] core::fx::FxExtent resolveEffectSize(const core::EffectSize& source, std::uint32_t defaultDimension,
                                                   bool defaultToScreen, const FxFrameContext& context,
                                                   const core::fx::FxResourceTable& table);

// Resolves declarations lazily across resource kinds, with cycle diagnostics.
// A supplied physical table takes precedence over declaration metadata.
class FxResourceSizeTable final : public core::fx::FxResourceTable {
  public:
    FxResourceSizeTable(const FxProgram& program, const FxFrameContext& context,
                        const core::fx::FxResourceTable* physical = nullptr);
    [[nodiscard]] std::optional<core::fx::FxExtent> find(std::string_view name) const override;
    void resolveAll() const;

  private:
    struct Node {
        core::EffectSize size;
        std::uint32_t dimension{};
        bool screenDefault{};
        bool sharedRef{};
        std::string filename;
    };
    std::unordered_map<std::string, Node> nodes_;
    mutable std::unordered_map<std::string, core::fx::FxExtent> resolved_;
    mutable std::vector<std::string> stack_;
    FxFrameContext context_;
    std::filesystem::path directory_;
    const core::fx::FxResourceTable* physical_{};
};
} // namespace dayo::fx
