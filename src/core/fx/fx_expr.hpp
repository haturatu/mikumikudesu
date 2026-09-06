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
struct FxEvalContext {
    std::int64_t rtWidth{};
    std::int64_t rtHeight{};
    std::int64_t vertexCount{};
    std::int64_t totalMaterial{};
    std::int64_t modelIndex{};
    std::int64_t cloneCount{1};
    std::int64_t clonedVertexCount{};
    std::int64_t frameIndex{};
    std::int64_t sampleIndex{};
};

// Bitmask describing which inputs an expression depends on.
enum class FxExprDependency : std::uint8_t {
    Static = 0,
    Resize = 1 << 0,
    Model = 1 << 1,
    Material = 1 << 2,
    Frame = 1 << 3,
};

[[nodiscard]] inline FxExprDependency operator|(FxExprDependency lhs, FxExprDependency rhs) noexcept {
    return static_cast<FxExprDependency>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

inline FxExprDependency& operator|=(FxExprDependency& lhs, FxExprDependency rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

[[nodiscard]] inline bool any(FxExprDependency mask, FxExprDependency bit) noexcept {
    return (static_cast<std::uint8_t>(mask) & static_cast<std::uint8_t>(bit)) != 0;
}

// Recursive AST. Nodes own children via shared_ptr so FxExpr stays copyable.
struct FxExpr {
    enum class BinaryOp : std::uint8_t {
        add,
        subtract,
        multiply,
        divide,
        modulo,
        less,
        lessEqual,
        greater,
        greaterEqual,
        equal,
        notEqual,
        logicalAnd,
        logicalOr,
    };

    struct Literal {
        FxScalar value{};
    };
    struct Identifier {
        std::string name;
    };
    struct Unary {
        char op{}; // '-' or '!'
        std::shared_ptr<FxExpr> operand;
    };
    struct Binary {
        BinaryOp op{BinaryOp::add};
        std::shared_ptr<FxExpr> lhs;
        std::shared_ptr<FxExpr> rhs;
    };
    struct Call {
        std::string name; // "pow" | "min" | "max"
        std::vector<std::shared_ptr<FxExpr>> args;
    };

    std::variant<Literal, Identifier, Unary, Binary, Call> node;
};

// Parse a scalar expression. Supports:
//   numbers, DEFAULT_RTSIZE.x/.y, VERTEXCOUNT, CLONEDVERTEXCOUNT,
//   TOTALMATERIAL, CloneCount, FRAME/FRAMEINDEX, SAMPLE/SAMPLEINDEX,
//   MODELINDEX, true/false, + - * / %, comparisons, &&, ||,
//   pow/min/max, parens, unary -/!.
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
