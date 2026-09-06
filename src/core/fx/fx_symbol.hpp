#pragma once

#include "core/fx/fx_compat.hpp"
#include "core/fx/fx_expr.hpp"
#include "core/fx/fx_size.hpp"

namespace dayo::core::fx {

// Unified CPU-side symbol resolution for FX semantic expressions.
// Handles builtins (DEFAULT_RTSIZE.x/.y, VERTEXCOUNT, CLONEDVERTEXCOUNT,
// TOTALMATERIAL, CloneCount, FRAME/SAMPLE/MODEL families) and named
// resources from an FxResourceTable. GPU code must not depend on this.
class FxSymbolResolver {
  public:
    FxSymbolResolver(const FxEvalContext& context, const FxResourceTable* table) noexcept
        : context_(&context), table_(table) {}

    // Scalar lookup used by expression evaluation with resource fallback.
    // Named resources resolve to their x component. Throws
    // std::runtime_error on unknown names.
    [[nodiscard]] FxScalar resolveScalar(std::string_view name) const;

    // Extent lookup used for size base inheritance. Builtins map to
    // synthetic extents (DEFAULT_RTSIZE -> rt size, VERTEXCOUNT family ->
    // 1D counts); otherwise falls back to the resource table.
    [[nodiscard]] FxExtent resolveExtent(std::string_view name) const;

    [[nodiscard]] const FxEvalContext& context() const noexcept {
        return *context_;
    }

  private:
    const FxEvalContext* context_;
    const FxResourceTable* table_;
};

// Evaluate with named-resource fallback: builtins come from the context,
// unknown identifiers resolve via resolver.resolveScalar().
[[nodiscard]] FxScalar evaluateFxExprWithSymbols(const FxExpr& expr, const FxSymbolResolver& resolver,
                                                 FxCompatibilityProfile profile = FxCompatibilityProfile::upstream130,
                                                 bool allowPowQuirk = false);

} // namespace dayo::core::fx
