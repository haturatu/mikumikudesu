#pragma once

#include "core/fx/fx_compat.hpp"
#include "core/fx/fx_expr.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace dayo::core::fx {

// Hard limits shared by textures and buffers. Resolver throws
// std::overflow_error before any GPU allocation when exceeded.
inline constexpr std::uint32_t kMaxTextureDimension = 16384;
inline constexpr std::uint64_t kMaxTextureBytes = 1024ULL * 1024ULL * 1024ULL; // 1GB
inline constexpr std::uint64_t kMaxBufferBytes = 1024ULL * 1024ULL * 1024ULL;  // 1GB
inline constexpr std::uint64_t kMaxElementCount = 1ULL << 30;
inline constexpr std::uint64_t kMaxTransientBytesPerEffect = 2ULL * 1024ULL * 1024ULL * 1024ULL; // 2GB

struct FxExtent {
    std::uint64_t x{1};
    std::uint64_t y{1};
    std::uint64_t z{1};
    std::uint32_t dimension{2};
};

// Semantic size expression. Resolution order is fixed:
// base -> dimension inheritance -> x/y/z expr -> conv -> ratio ->
// rounding -> overflow validation.
struct FxSizeExpr {
    enum class Rounding {
        nearest,
        floor,
        ceil,
    };

    std::string base;          // "" = absolute, otherwise resource or builtin name
    std::uint32_t dimension{}; // 0 = inherit from base (default 2 when no base)
    std::string xExpr;         // "" = inherit from base
    std::string yExpr;
    std::string zExpr;
    // Unit-conversion scale applied right after expression evaluation
    // (FXSize::convert stage), before the output ratio.
    float convX{1.0F};
    float convY{1.0F};
    float convZ{1.0F};
    float widthRatio{1.0F};
    float heightRatio{1.0F};
    float depthRatio{1.0F};
    Rounding rounding{Rounding::nearest};
};

// Read-only view of already resolved resources (for base inheritance).
class FxResourceTable {
  public:
    virtual ~FxResourceTable() = default;
    [[nodiscard]] virtual std::optional<FxExtent> find(std::string_view name) const = 0;
};

class FakeFxResourceTable final : public FxResourceTable {
  public:
    void add(std::string name, FxExtent extent) {
        entries_.insert_or_assign(std::move(name), extent);
    }

    [[nodiscard]] std::optional<FxExtent> find(std::string_view name) const override {
        const auto found = entries_.find(std::string(name));
        if (found == entries_.end())
            return std::nullopt;
        return found->second;
    }

  private:
    std::unordered_map<std::string, FxExtent> entries_;
};

class FxSizeResolver {
  public:
    // Resolve an FxSizeExpr to a concrete extent. Throws std::runtime_error
    // on unknown base/identifier and std::overflow_error on any limit breach.
    [[nodiscard]] FxExtent resolve(const FxSizeExpr& expr, const FxEvalContext& context, const FxResourceTable& table,
                                   FxCompatibilityProfile profile = FxCompatibilityProfile::upstream130,
                                   bool allowPowQuirk = false) const;

    // Byte helpers with checked arithmetic. Throw std::overflow_error when
    // the corresponding kMax* limit is exceeded.
    [[nodiscard]] static std::uint64_t textureBytes(const FxExtent& extent, std::uint64_t bytesPerTexel);
    [[nodiscard]] static std::uint64_t bufferBytes(const FxExtent& extent, std::uint64_t elementSize);
    [[nodiscard]] static std::uint64_t elementCount(const FxExtent& extent);
    // Per-effect transient budget guard. Callers sum the bytes of every
    // transient resource in one effect and validate the total here so a
    // single giant allocation plan is rejected before any GPU work.
    static void checkTransientBytes(std::uint64_t bytes);
};

// Checked integer helpers used by the resolver (also unit-tested indirectly).
[[nodiscard]] std::uint64_t fxCheckedMul(std::uint64_t lhs, std::uint64_t rhs);
[[nodiscard]] std::uint64_t fxCheckedAdd(std::uint64_t lhs, std::uint64_t rhs);

} // namespace dayo::core::fx
