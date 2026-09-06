#pragma once

#include "core/fx/fx_compat.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dayo::core::fx {

// Scalar value produced by FX size/condition expressions.
using FxScalar = std::variant<std::int64_t, double, bool>;

// CPU-side evaluation inputs. GPU code must not depend on this struct.
// Types mirror the runtime ABI (TASKS.md section 11): render size and
// per-model indices are 32-bit, accumulated counts/indices are 64-bit.
struct FxEvalContext {
    std::uint32_t rtWidth{};
    std::uint32_t rtHeight{};

    std::uint64_t vertexCount{};
    std::uint64_t totalMaterial{};

    std::uint32_t modelIndex{};
    std::uint32_t cloneCount{1};
    std::uint64_t clonedVertexCount{};

    std::uint64_t frameIndex{};
    std::uint64_t sampleIndex{};
};

// Bitmask describing which inputs an expression depends on, so the
// FxInstance cache can skip re-evaluation when inputs are unchanged.
enum class FxExprDependency : std::uint32_t {
    StaticDependency = 0,
    ResizeDependency = 1 << 0,
    ModelDependency = 1 << 1,
    MaterialDependency = 1 << 2,
    FrameDependency = 1 << 3,
};

[[nodiscard]] inline FxExprDependency operator|(FxExprDependency lhs, FxExprDependency rhs) noexcept {
    return static_cast<FxExprDependency>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

inline FxExprDependency& operator|=(FxExprDependency& lhs, FxExprDependency rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

[[nodiscard]] inline bool any(FxExprDependency mask, FxExprDependency bit) noexcept {
    return (static_cast<std::uint32_t>(mask) & static_cast<std::uint32_t>(bit)) != 0;
}

// Recursive AST with parse-once/evaluate-on-demand semantics. Nodes live at
// namespace scope (rather than nested in FxExpr) so std::variant sees
// complete alternatives; children are shared_ptr so FxExpr stays copyable.
struct FxExpr;

struct FxLiteralExpr {
    FxScalar value{static_cast<std::int64_t>(0)};
};

struct FxIdentifierExpr {
    std::string name;
};

struct FxUnaryExpr {
    char op{}; // '-' or '!'
    std::shared_ptr<FxExpr> operand;
};

struct FxBinaryExpr {
    char op{}; // '+' '-' '*' '/' '%'
    std::shared_ptr<FxExpr> lhs;
    std::shared_ptr<FxExpr> rhs;
};

struct FxCallExpr {
    std::string name; // "pow" | "min" | "max"
    std::vector<std::shared_ptr<FxExpr>> args;
};

struct FxExpr {
    std::variant<FxLiteralExpr, FxIdentifierExpr, FxUnaryExpr, FxBinaryExpr, FxCallExpr> node;
};

// Parse a scalar expression. Supports:
//   numbers, DEFAULT_RTSIZE.x/.y, VERTEXCOUNT, CLONEDVERTEXCOUNT,
//   TOTALMATERIAL, CloneCount, FRAME/FRAMEINDEX, SAMPLE/SAMPLEINDEX,
//   MODELINDEX, true/false, + - * / %, pow/min/max, parens, unary -/!.
// Throws std::runtime_error on syntax errors. Profile-independent: pow
// gating happens at evaluate() time so one AST serves both profiles.
[[nodiscard]] FxExpr parseFxExpr(std::string_view text);

// Evaluate an AST. pow() handling is profile-separated:
//   upstream130     -> only when allowPowQuirk is true (quirk allowlist path)
//   nativeExtended  -> always allowed
// Throws std::runtime_error on unknown identifiers, arity errors,
// division by zero, or disallowed pow().
[[nodiscard]] FxScalar evaluateFxExpr(const FxExpr& expr, const FxEvalContext& context,
                                      FxCompatibilityProfile profile = FxCompatibilityProfile::upstream130,
                                      bool allowPowQuirk = false);

// Bitmask OR of every input the expression reads.
[[nodiscard]] FxExprDependency fxDependencies(const FxExpr& expr) noexcept;

// Conversions used by size resolution and tests.
[[nodiscard]] double fxToDouble(const FxScalar& value) noexcept;
[[nodiscard]] std::int64_t fxToInt(const FxScalar& value);
[[nodiscard]] bool fxToBool(const FxScalar& value) noexcept;

} // namespace dayo::core::fx
