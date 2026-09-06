#include "core/fx/fx_size.hpp"

#include "core/fx/fx_symbol.hpp"
#include "core/log.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace dayo::core::fx {
namespace {

[[nodiscard]] std::int64_t toRoundedInt(double scaled, FxSizeExpr::Rounding rounding, std::string_view axis) {
    if (!std::isfinite(scaled))
        throw std::overflow_error("fx size is not finite: " + std::string(axis));
    // DoS guard: reject magnitudes that can never fit before llround().
    constexpr double kReject = 4.0e9;
    if (scaled >= kReject || scaled <= -kReject)
        throw std::overflow_error("fx size out of range: " + std::string(axis));
    long long rounded = 0;
    switch (rounding) {
    case FxSizeExpr::Rounding::nearest:
        rounded = std::llround(scaled);
        break;
    case FxSizeExpr::Rounding::floor:
        rounded = static_cast<long long>(std::floor(scaled));
        break;
    case FxSizeExpr::Rounding::ceil:
        rounded = static_cast<long long>(std::ceil(scaled));
        break;
    }
    if (rounded <= 0)
        throw std::runtime_error("fx size must be positive: " + std::string(axis));
    if (rounded > static_cast<long long>(std::numeric_limits<std::uint32_t>::max()))
        throw std::overflow_error("fx size out of range: " + std::string(axis));
    return static_cast<std::int64_t>(rounded);
}

[[nodiscard]] std::int64_t resolveAxis(std::string_view exprText, std::int64_t inherited, float ratio,
                                       FxSizeExpr::Rounding rounding, std::string_view axis,
                                       const FxEvalContext& context, const FxResourceTable& table,
                                       FxCompatibilityProfile profile, bool allowPowQuirk) {
    double raw = 0.0;
    bool hasValue = false;
    if (!exprText.empty()) {
        const FxExpr parsed = parseFxExpr(exprText);
        const FxSymbolResolver resolver(context, &table);
        const FxScalar value = evaluateFxExprWithSymbols(parsed, resolver, profile, allowPowQuirk);
        // conv: scalar -> double. fxToInt-equivalent range check happens via
        // finiteness + reject window below; keep double for ratio first so
        // "conv -> ratio -> rounding" order matches the spec narrative
        // (integer truncation before ratio would lose fractional ratios).
        raw = fxToDouble(value);
        if (!std::isfinite(raw))
            throw std::overflow_error("fx size expression is not finite: " + std::string(axis));
        hasValue = true;
    } else if (inherited > 0) {
        raw = static_cast<double>(inherited);
        hasValue = true;
    }
    if (!hasValue)
        throw std::runtime_error("fx size is missing value: " + std::string(axis));
    if (!std::isfinite(ratio) || ratio <= 0.0F)
        throw std::runtime_error("fx size ratio must be positive finite: " + std::string(axis));
    const double scaled = raw * static_cast<double>(ratio);
    return toRoundedInt(scaled, rounding, axis);
}

} // namespace

std::uint64_t fxCheckedMul(std::uint64_t lhs, std::uint64_t rhs) {
    std::uint64_t out{};
    if (__builtin_mul_overflow(lhs, rhs, &out))
        throw std::overflow_error("fx size multiply overflow");
    return out;
}

std::uint64_t fxCheckedAdd(std::uint64_t lhs, std::uint64_t rhs) {
    std::uint64_t out{};
    if (__builtin_add_overflow(lhs, rhs, &out))
        throw std::overflow_error("fx size add overflow");
    return out;
}

std::uint64_t FxSizeResolver::elementCount(const FxExtent& extent) {
    const std::uint64_t xy = fxCheckedMul(extent.x, extent.y);
    const std::uint64_t xyz = fxCheckedMul(xy, extent.z);
    if (xyz > kMaxElementCount || xyz == 0)
        throw std::overflow_error("fx element count out of range");
    return xyz;
}

std::uint64_t FxSizeResolver::textureBytes(const FxExtent& extent, std::uint64_t bytesPerTexel) {
    if (bytesPerTexel == 0)
        throw std::runtime_error("fx texture bytes-per-texel must be positive");
    if (extent.dimension < 1 || extent.dimension > 3)
        throw std::runtime_error("fx texture dimension out of range");
    if (extent.x == 0 || extent.x > kMaxTextureDimension)
        throw std::overflow_error("fx texture width out of range");
    if (extent.dimension >= 2 && (extent.y == 0 || extent.y > kMaxTextureDimension))
        throw std::overflow_error("fx texture height out of range");
    if (extent.dimension >= 3 && (extent.z == 0 || extent.z > kMaxTextureDimension))
        throw std::overflow_error("fx texture depth out of range");
    const std::uint64_t count = elementCount(extent);
    const std::uint64_t bytes = fxCheckedMul(count, bytesPerTexel);
    if (bytes > kMaxTextureBytes)
        throw std::overflow_error("fx texture bytes out of range");
    return bytes;
}

std::uint64_t FxSizeResolver::bufferBytes(const FxExtent& extent, std::uint64_t elementSize) {
    if (elementSize == 0)
        throw std::runtime_error("fx buffer element size must be positive");
    const std::uint64_t count = elementCount(extent);
    const std::uint64_t bytes = fxCheckedMul(count, elementSize);
    if (bytes > kMaxBufferBytes)
        throw std::overflow_error("fx buffer bytes out of range");
    return bytes;
}

FxExtent FxSizeResolver::resolve(const FxSizeExpr& expr, const FxEvalContext& context, const FxResourceTable& table,
                                 FxCompatibilityProfile profile, bool allowPowQuirk) const {
    // 1. base
    std::optional<FxExtent> base;
    if (!expr.base.empty()) {
        const FxSymbolResolver resolver(context, &table);
        base = resolver.resolveExtent(expr.base);
        if (base->dimension < 1 || base->dimension > 3) {
            dayo::log::warn("fx size base has invalid dimension: ", expr.base);
            throw std::runtime_error("fx size base has invalid dimension: " + expr.base);
        }
        if (base->x == 0 || base->y == 0 || base->z == 0) {
            dayo::log::warn("fx size base has empty extent: ", expr.base);
            throw std::runtime_error("fx size base has empty extent: " + expr.base);
        }
    }

    // 2. dimension inheritance
    std::uint32_t dimension = expr.dimension;
    if (dimension == 0)
        dimension = base.has_value() ? base->dimension : 2;
    if (dimension < 1 || dimension > 3) {
        dayo::log::warn("fx size dimension out of range");
        throw std::runtime_error("fx size dimension out of range");
    }

    const std::int64_t baseX = base.has_value() ? static_cast<std::int64_t>(base->x) : 0;
    const std::int64_t baseY = base.has_value() ? static_cast<std::int64_t>(base->y) : 0;
    const std::int64_t baseZ = base.has_value() ? static_cast<std::int64_t>(base->z) : 0;

    // 3-6. x/y/z expr -> conv -> ratio -> rounding (per axis)
    FxExtent out;
    out.dimension = dimension;
    const std::int64_t x = resolveAxis(expr.xExpr, baseX, expr.widthRatio, expr.rounding, "x", context, table,
                                       profile, allowPowQuirk);
    std::int64_t y = 1;
    std::int64_t z = 1;
    if (dimension >= 2)
        y = resolveAxis(expr.yExpr, baseY, expr.heightRatio, expr.rounding, "y", context, table, profile,
                        allowPowQuirk);
    if (dimension >= 3)
        z = resolveAxis(expr.zExpr, baseZ, 1.0F, expr.rounding, "z", context, table, profile, allowPowQuirk);

    // 7. overflow validation
    if (dimension == 1) {
        if (x <= 0 || static_cast<std::uint64_t>(x) > kMaxElementCount)
            throw std::overflow_error("fx buffer element count out of range");
        out.x = static_cast<std::uint32_t>(x);
        out.y = 1;
        out.z = 1;
    } else {
        if (x <= 0 || static_cast<std::uint64_t>(x) > kMaxTextureDimension)
            throw std::overflow_error("fx texture width out of range");
        if (y <= 0 || static_cast<std::uint64_t>(y) > kMaxTextureDimension)
            throw std::overflow_error("fx texture height out of range");
        out.x = static_cast<std::uint32_t>(x);
        out.y = static_cast<std::uint32_t>(y);
        out.z = 1;
        if (dimension == 3) {
            if (z <= 0 || static_cast<std::uint64_t>(z) > kMaxTextureDimension)
                throw std::overflow_error("fx texture depth out of range");
            out.z = static_cast<std::uint32_t>(z);
        }
    }
    // Total element guard (DoS prevention for 16384^3-class extents).
    static_cast<void>(elementCount(out));
    dayo::log::debug("fx size resolved dim=", out.dimension, " extent=", out.x, "x", out.y, "x", out.z);
    return out;
}

} // namespace dayo::core::fx
