#pragma once

#include "core/fx/fx_condition.hpp"
#include "core/fx/fx_expr.hpp"
#include "core/fx/fx_size.hpp"
#include "fx/fx_frame.hpp"

#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace dayo::fx {

// Frame-local evaluator for compiled pass conditions. Parsed predicates are
// cached per effect instance; resources are looked up from that instance's
// resolved resource runtime.
class FxConditionRuntime {
  public:
    [[nodiscard]] bool evaluate(std::span<const std::string> conditions, const FxFrameContext& context,
                                const core::fx::FxResourceTable* resources = nullptr);
    void clear() noexcept {
        compiled_.clear();
    }

  private:
    struct CompiledCondition {
        core::fx::FxEventMask events{};
        std::optional<core::fx::FxExpr> predicate;
    };

    [[nodiscard]] const CompiledCondition& compile(const std::string& source);

    std::unordered_map<std::string, CompiledCondition> compiled_;
};

} // namespace dayo::fx
