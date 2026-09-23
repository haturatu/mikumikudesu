#include "fx/fx_condition_runtime.hpp"

#include "core/fx/fx_symbol.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dayo::fx {
namespace {

std::int64_t clampSigned(std::uint64_t value) noexcept {
    constexpr auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return value > limit ? std::numeric_limits<std::int64_t>::max() : static_cast<std::int64_t>(value);
}

std::int64_t frameIndex(float frame) noexcept {
    if (!std::isfinite(frame))
        return 0;
    const auto value = std::floor(static_cast<double>(frame));
    if (value >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
        return std::numeric_limits<std::int64_t>::max();
    if (value <= static_cast<double>(std::numeric_limits<std::int64_t>::min()))
        return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(value);
}

core::fx::FxEventMask activeEvents(const FxHostFrameState& host) noexcept {
    auto result = core::fx::kFxEventFrame;
    if (host.onLoad)
        result |= core::fx::kFxEventLoad;
    if (host.onStart)
        result |= core::fx::kFxEventStart;
    if (host.onResize)
        result |= core::fx::kFxEventResize;
    if (host.onModelChanged)
        result |= core::fx::kFxEventModelChanged;
    if (host.onMaterialChanged)
        result |= core::fx::kFxEventMaterialChanged;
    return result;
}

core::fx::FxEvalContext expressionContext(const FxFrameContext& context) noexcept {
    return {.rtWidth = context.renderWidth,
            .rtHeight = context.renderHeight,
            .vertexCount = clampSigned(context.vertexCount),
            .totalMaterial = clampSigned(context.totalMaterial),
            .modelIndex = context.modelIndex,
            .cloneCount = context.cloneCount,
            .clonedVertexCount = clampSigned(context.clonedVertexCount),
            .frameIndex = frameIndex(context.frame),
            .sampleIndex = clampSigned(context.sample)};
}

} // namespace

const FxConditionRuntime::CompiledCondition& FxConditionRuntime::compile(const std::string& source) {
    if (const auto found = compiled_.find(source); found != compiled_.end())
        return found->second;

    const auto parsed = core::fx::compileFxCondition(source);
    CompiledCondition result{.events = parsed.events, .predicate = std::nullopt};
    if (!parsed.predicate.empty())
        result.predicate = core::fx::parseFxExpr(parsed.predicate);
    return compiled_.emplace(source, std::move(result)).first->second;
}

bool FxConditionRuntime::evaluate(std::span<const std::string> conditions, const FxFrameContext& context,
                                  const core::fx::FxResourceTable* resources) {
    const auto active = activeEvents(context.host);
    const auto values = expressionContext(context);
    const core::fx::FxSymbolResolver resolver(values, resources);
    return std::ranges::all_of(conditions, [this, active, &resolver](const std::string& source) {
        const auto& condition = compile(source);
        if (condition.events != core::fx::kFxEventNone && (condition.events & active) == 0)
            return false;
        return !condition.predicate.has_value() ||
               core::fx::fxToBool(core::fx::evaluateFxExprWithSymbols(*condition.predicate, resolver));
    });
}

} // namespace dayo::fx
