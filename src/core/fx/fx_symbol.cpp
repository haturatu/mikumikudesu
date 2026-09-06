#include "core/fx/fx_symbol.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace dayo::core::fx {
namespace {

bool builtinScalar(std::string_view name, const FxEvalContext& context, FxScalar& out) noexcept {
    if (name == "DEFAULT_RTSIZE.x") {
        out = context.rtWidth;
        return true;
    }
    if (name == "DEFAULT_RTSIZE.y") {
        out = context.rtHeight;
        return true;
    }
    if (name == "VERTEXCOUNT") {
        out = context.vertexCount;
        return true;
    }
    if (name == "CLONEDVERTEXCOUNT") {
        out = context.clonedVertexCount;
        return true;
    }
    if (name == "TOTALMATERIAL") {
        out = context.totalMaterial;
        return true;
    }
    if (name == "CloneCount" || name == "CLONECOUNT") {
        out = context.cloneCount;
        return true;
    }
    if (name == "MODELINDEX") {
        out = context.modelIndex;
        return true;
    }
    if (name == "FRAME" || name == "FRAMEINDEX") {
        out = context.frameIndex;
        return true;
    }
    if (name == "SAMPLE" || name == "SAMPLEINDEX") {
        out = context.sampleIndex;
        return true;
    }
    return false;
}

FxScalar evalWithResolver(const FxExpr& expr, const FxSymbolResolver& resolver, FxCompatibilityProfile profile,
                          bool allowPowQuirk);

FxScalar evalBinaryScalar(FxExpr::BinaryOp op, const FxScalar& lhs, const FxScalar& rhs) {
    // Reuse the same semantics as fx_expr evaluation for consistency.
    // Integer path uses checked arithmetic to surface overflow early.
    const bool useDouble = std::holds_alternative<double>(lhs) || std::holds_alternative<double>(rhs);
    const auto asInt = [](const FxScalar& value) -> std::int64_t {
        if (const auto* b = std::get_if<bool>(&value))
            return *b ? 1 : 0;
        return std::get<std::int64_t>(value);
    };
    const double leftDouble = fxToDouble(lhs);
    const double rightDouble = fxToDouble(rhs);
    const std::int64_t leftInt = asInt(lhs);
    const std::int64_t rightInt = asInt(rhs);
    switch (op) {
    case FxExpr::BinaryOp::less:
        return FxScalar{useDouble ? leftDouble < rightDouble : leftInt < rightInt};
    case FxExpr::BinaryOp::lessEqual:
        return FxScalar{useDouble ? leftDouble <= rightDouble : leftInt <= rightInt};
    case FxExpr::BinaryOp::greater:
        return FxScalar{useDouble ? leftDouble > rightDouble : leftInt > rightInt};
    case FxExpr::BinaryOp::greaterEqual:
        return FxScalar{useDouble ? leftDouble >= rightDouble : leftInt >= rightInt};
    case FxExpr::BinaryOp::equal:
        return FxScalar{useDouble ? leftDouble == rightDouble : leftInt == rightInt};
    case FxExpr::BinaryOp::notEqual:
        return FxScalar{useDouble ? leftDouble != rightDouble : leftInt != rightInt};
    case FxExpr::BinaryOp::logicalAnd:
        return FxScalar{fxToBool(lhs) && fxToBool(rhs)};
    case FxExpr::BinaryOp::logicalOr:
        return FxScalar{fxToBool(lhs) || fxToBool(rhs)};
    default:
        break;
    }
    const bool lhsDouble = std::holds_alternative<double>(lhs);
    const bool rhsDouble = std::holds_alternative<double>(rhs);
    if (lhsDouble || rhsDouble) {
        const double left = fxToDouble(lhs);
        const double right = fxToDouble(rhs);
        switch (op) {
        case FxExpr::BinaryOp::add:
            return FxScalar{left + right};
        case FxExpr::BinaryOp::subtract:
            return FxScalar{left - right};
        case FxExpr::BinaryOp::multiply:
            return FxScalar{left * right};
        case FxExpr::BinaryOp::divide:
            if (right == 0.0)
                throw std::runtime_error("fx expression division by zero");
            return FxScalar{left / right};
        case FxExpr::BinaryOp::modulo:
            if (right == 0.0)
                throw std::runtime_error("fx expression modulo by zero");
            return FxScalar{std::fmod(left, right)};
        default:
            throw std::runtime_error("fx expression has unknown binary operator");
        }
    }
    const std::int64_t left = asInt(lhs);
    const std::int64_t right = asInt(rhs);
    std::int64_t out{};
    switch (op) {
    case FxExpr::BinaryOp::add:
        if (__builtin_add_overflow(left, right, &out))
            throw std::overflow_error("fx expression integer overflow");
        return FxScalar{out};
    case FxExpr::BinaryOp::subtract:
        if (__builtin_sub_overflow(left, right, &out))
            throw std::overflow_error("fx expression integer overflow");
        return FxScalar{out};
    case FxExpr::BinaryOp::multiply:
        if (__builtin_mul_overflow(left, right, &out))
            throw std::overflow_error("fx expression integer overflow");
        return FxScalar{out};
    case FxExpr::BinaryOp::divide:
        if (right == 0)
            throw std::runtime_error("fx expression division by zero");
        if (left == std::numeric_limits<std::int64_t>::min() && right == -1)
            throw std::overflow_error("fx expression integer overflow");
        return FxScalar{left / right};
    case FxExpr::BinaryOp::modulo:
        if (right == 0)
            throw std::runtime_error("fx expression modulo by zero");
        return FxScalar{left % right};
    default:
        throw std::runtime_error("fx expression has unknown binary operator");
    }
}

FxScalar evalCallScalar(const FxExpr::Call& call, const FxSymbolResolver& resolver, FxCompatibilityProfile profile,
                        bool allowPowQuirk) {
    if (call.name == "pow") {
        if (call.args.size() != 2)
            throw std::runtime_error("pow() expects 2 arguments");
        if (profile == FxCompatibilityProfile::upstream130 && !allowPowQuirk) {
            dayo::log::warn("fx pow() rejected under upstream130 without quirk allowlist");
            throw std::runtime_error("pow() requires quirk allowlist under upstream130");
        }
        const double base = fxToDouble(evalWithResolver(*call.args[0], resolver, profile, allowPowQuirk));
        const double exp = fxToDouble(evalWithResolver(*call.args[1], resolver, profile, allowPowQuirk));
        return FxScalar{std::pow(base, exp)};
    }
    if (call.name == "min" || call.name == "max") {
        if (call.args.size() < 2)
            throw std::runtime_error(call.name + "() expects at least 2 arguments");
        const bool wantMin = call.name == "min";
        bool hasDouble = false;
        for (const auto& arg : call.args) {
            if (std::holds_alternative<double>(evalWithResolver(*arg, resolver, profile, allowPowQuirk)))
                hasDouble = true;
        }
        if (hasDouble) {
            double best = fxToDouble(evalWithResolver(*call.args[0], resolver, profile, allowPowQuirk));
            for (std::size_t i = 1; i < call.args.size(); ++i) {
                const double v = fxToDouble(evalWithResolver(*call.args[i], resolver, profile, allowPowQuirk));
                best = wantMin ? std::fmin(best, v) : std::fmax(best, v);
            }
            return FxScalar{best};
        }
        const auto asInt = [](const FxScalar& value) -> std::int64_t {
            if (const auto* b = std::get_if<bool>(&value))
                return *b ? 1 : 0;
            return std::get<std::int64_t>(value);
        };
        std::int64_t best = asInt(evalWithResolver(*call.args[0], resolver, profile, allowPowQuirk));
        for (std::size_t i = 1; i < call.args.size(); ++i) {
            const std::int64_t v = asInt(evalWithResolver(*call.args[i], resolver, profile, allowPowQuirk));
            best = wantMin ? std::min(best, v) : std::max(best, v);
        }
        return FxScalar{best};
    }
    throw std::runtime_error("fx expression has unknown function: " + call.name);
}

FxScalar evalWithResolver(const FxExpr& expr, const FxSymbolResolver& resolver, FxCompatibilityProfile profile,
                          bool allowPowQuirk) {
    if (const auto* lit = std::get_if<FxExpr::Literal>(&expr.node))
        return lit->value;
    if (const auto* ident = std::get_if<FxExpr::Identifier>(&expr.node))
        return resolver.resolveScalar(ident->name);
    if (const auto* unary = std::get_if<FxExpr::Unary>(&expr.node)) {
        const FxScalar v = evalWithResolver(*unary->operand, resolver, profile, allowPowQuirk);
        if (unary->op == '!')
            return FxScalar{!fxToBool(v)};
        if (unary->op == '-') {
            if (const auto* d = std::get_if<double>(&v))
                return FxScalar{-(*d)};
            if (const auto* b = std::get_if<bool>(&v))
                return FxScalar{(*b) ? std::int64_t{-1} : std::int64_t{0}};
            const std::int64_t i = std::get<std::int64_t>(v);
            if (i == std::numeric_limits<std::int64_t>::min())
                throw std::overflow_error("fx expression integer overflow");
            return FxScalar{-i};
        }
        throw std::runtime_error("fx expression has unknown unary operator");
    }
    if (const auto* binary = std::get_if<FxExpr::Binary>(&expr.node)) {
        const FxScalar lhs = evalWithResolver(*binary->lhs, resolver, profile, allowPowQuirk);
        if (binary->op == FxExpr::BinaryOp::logicalAnd && !fxToBool(lhs))
            return FxScalar{false};
        if (binary->op == FxExpr::BinaryOp::logicalOr && fxToBool(lhs))
            return FxScalar{true};
        const FxScalar rhs = evalWithResolver(*binary->rhs, resolver, profile, allowPowQuirk);
        return evalBinaryScalar(binary->op, lhs, rhs);
    }
    if (const auto* call = std::get_if<FxExpr::Call>(&expr.node))
        return evalCallScalar(*call, resolver, profile, allowPowQuirk);
    throw std::runtime_error("fx expression has invalid node");
}

[[nodiscard]] std::uint32_t toUint32Checked(std::int64_t value, std::string_view what) {
    if (value <= 0)
        throw std::runtime_error("fx symbol has non-positive extent: " + std::string(what));
    if (value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()))
        throw std::overflow_error("fx symbol extent out of range: " + std::string(what));
    return static_cast<std::uint32_t>(value);
}

} // namespace

FxScalar FxSymbolResolver::resolveScalar(std::string_view name) const {
    FxScalar out{};
    if (builtinScalar(name, *context_, out))
        return out;
    if (table_ != nullptr) {
        if (const auto found = table_->find(name); found.has_value()) {
            // Named resources decay to their x component in scalar context.
            // This keeps size expressions and condition expressions sharing
            // one namespace without introducing a vector scalar type.
            return FxScalar{static_cast<std::int64_t>(found->x)};
        }
    }
    dayo::log::warn("fx symbol has unknown identifier: ", name);
    throw std::runtime_error("fx symbol has unknown identifier: " + std::string(name));
}

FxExtent FxSymbolResolver::resolveExtent(std::string_view name) const {
    if (name == "DEFAULT_RTSIZE") {
        FxExtent extent;
        extent.x = toUint32Checked(context_->rtWidth, name);
        extent.y = toUint32Checked(context_->rtHeight, name);
        extent.z = 1;
        extent.dimension = 2;
        return extent;
    }
    if (name == "VERTEXCOUNT") {
        FxExtent extent;
        extent.x = toUint32Checked(context_->vertexCount, name);
        extent.y = 1;
        extent.z = 1;
        extent.dimension = 1;
        return extent;
    }
    if (name == "CLONEDVERTEXCOUNT") {
        FxExtent extent;
        extent.x = toUint32Checked(context_->clonedVertexCount, name);
        extent.y = 1;
        extent.z = 1;
        extent.dimension = 1;
        return extent;
    }
    if (name == "TOTALMATERIAL") {
        FxExtent extent;
        extent.x = toUint32Checked(context_->totalMaterial, name);
        extent.y = 1;
        extent.z = 1;
        extent.dimension = 1;
        return extent;
    }
    if (table_ != nullptr) {
        if (const auto found = table_->find(name); found.has_value())
            return *found;
    }
    dayo::log::warn("fx symbol has unknown resource: ", name);
    throw std::runtime_error("fx symbol has unknown resource: " + std::string(name));
}

FxScalar evaluateFxExprWithSymbols(const FxExpr& expr, const FxSymbolResolver& resolver, FxCompatibilityProfile profile,
                                   bool allowPowQuirk) {
    return evalWithResolver(expr, resolver, profile, allowPowQuirk);
}

} // namespace dayo::core::fx
