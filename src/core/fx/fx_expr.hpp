#pragma once

#include "core/fx/fx_compat.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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
    double time{};
    std::unordered_map<std::string, FxScalar> namedSymbols;
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
        std::string name;
        std::vector<std::shared_ptr<FxExpr>> args;
    };

    std::variant<Literal, Identifier, Unary, Binary, Call> node;

    // Keep construction portable across standard library implementations:
    // the recursive alternatives do not need to determine variant's default
    // alternative.
    FxExpr() : node(Literal{FxScalar{std::int64_t{0}}}) {}
};

// Parse a scalar expression. Supports:
//   numbers, DEFAULT_RTSIZE.x/.y, VERTEXCOUNT, CLONEDVERTEXCOUNT,
//   TOTALMATERIAL, CloneCount, FRAME/FRAMEINDEX, SAMPLE/SAMPLEINDEX,
//   MODELINDEX, true/false, + - * / %, comparisons, &&, ||,
//   the scalar functions supported by MikuMikuDayo 1.30 Expr.ixx,
//   parens, unary -/!.
// Throws std::runtime_error on syntax errors.
[[nodiscard]] FxExpr parseFxExpr(std::string_view text);
[[nodiscard]] bool isSupportedFxFunction(std::string_view name) noexcept;
[[nodiscard]] FxScalar evaluateFxFunction(std::string_view name, std::span<const FxScalar> arguments);

// Evaluate an AST. The compatibility parameters remain for size-resolution
// callers; the function set follows MikuMikuDayo 1.30 Expr.ixx in both profiles.
// Throws std::runtime_error on unknown identifiers, arity errors, or invalid math.
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
