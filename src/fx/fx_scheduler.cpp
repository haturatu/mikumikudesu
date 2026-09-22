#include "fx/fx_scheduler.hpp"

#include "core/fx/fx_pass.hpp"

#include <algorithm>
#include <stdexcept>

namespace dayo::fx {

const char* toString(FrameStage stage) noexcept {
    switch (stage) {
    case FrameStage::deform:
        return "deform";
    case FrameStage::renderer:
        return "renderer";
    case FrameStage::postPre:
        return "postPre";
    case FrameStage::tonemap:
        return "tonemap";
    case FrameStage::postPost:
        return "postPost";
    case FrameStage::present:
        return "present";
    }
    return "renderer";
}

void FrameEffectScheduler::setControllerEnabled(std::string name, bool enabled) {
    if (enabled)
        controllerOff_.erase(name);
    else
        controllerOff_[std::move(name)] = false;
}

bool FrameEffectScheduler::isEnabled(const std::string& name) const noexcept {
    return !controllerOff_.contains(name);
}

FrameStage FrameEffectScheduler::stageFor(const FxCatalogEntry& entry) noexcept {
    switch (entry.executionCategory) {
    case core::fx::FxCategory::deform:
        return FrameStage::deform;
    case core::fx::FxCategory::render:
        return FrameStage::renderer;
    case core::fx::FxCategory::postprocess:
        return entry.executionOrder < 100 ? FrameStage::postPre : FrameStage::postPost;
    }
    return FrameStage::renderer;
}

std::vector<ScheduledFx> FrameEffectScheduler::schedule(const EffectCatalog& catalog,
                                                        const std::string& rendererName) const {
    std::vector<ScheduledFx> result;
    // Deform covers every model before any renderer work.
    result.push_back({"deform", FrameStage::deform, -1000});

    std::vector<FxCatalogEntry> renderers;
    std::vector<FxCatalogEntry> posts;
    for (const auto& entry : catalog.all()) {
        if (!isEnabled(entry.name) || !entry.controllerEnabled)
            continue;
        if (entry.executionCategory == core::fx::FxCategory::postprocess)
            posts.push_back(entry);
        else if (entry.executionCategory == core::fx::FxCategory::deform)
            result.push_back({entry.name, FrameStage::deform, entry.executionOrder});
        else
            renderers.push_back(entry);
    }
    // Active renderer first, then remaining renderer/particle/sample in order.
    std::sort(renderers.begin(), renderers.end(),
              [](const auto& left, const auto& right) { return left.executionOrder < right.executionOrder; });
    bool emittedActive = rendererName.empty();
    if (!rendererName.empty()) {
        for (const auto& entry : renderers) {
            if (entry.name == rendererName) {
                result.push_back({entry.name, FrameStage::renderer, entry.executionOrder});
                emittedActive = true;
                break;
            }
        }
    }
    for (const auto& entry : renderers) {
        if (!rendererName.empty() && entry.name == rendererName)
            continue;
        result.push_back({entry.name, FrameStage::renderer, entry.executionOrder});
    }
    static_cast<void>(emittedActive);

    std::sort(posts.begin(), posts.end(),
              [](const auto& left, const auto& right) { return left.executionOrder < right.executionOrder; });
    for (const auto& entry : posts)
        result.push_back({entry.name, stageFor(entry), entry.executionOrder});

    // Stable stage ordering: deform -> renderer -> postPre -> postPost ->
    // present. Within a stage keep executionOrder. Tonemap is not inferred
    // from a filename; an upstream postprocess graph owns its own metadata.
    const auto rank = [](FrameStage stage) {
        switch (stage) {
        case FrameStage::deform:
            return 0;
        case FrameStage::renderer:
            return 1;
        case FrameStage::postPre:
            return 2;
        case FrameStage::tonemap:
            return 3;
        case FrameStage::postPost:
            return 4;
        case FrameStage::present:
            return 5;
        }
        return 1;
    };
    std::stable_sort(result.begin(), result.end(), [&](const ScheduledFx& left, const ScheduledFx& right) {
        const int leftRank = rank(left.stage);
        const int rightRank = rank(right.stage);
        if (leftRank != rightRank)
            return leftRank < rightRank;
        return left.order < right.order;
    });

    result.push_back({"present", FrameStage::present, 1000});
    return result;
}

std::vector<ScheduledFx> FrameEffectScheduler::schedule(const core::SceneEffectStack& effects,
                                                        const ModelOrderLookup& modelOrder) const {
    std::vector<ScheduledFx> result;
    result.push_back({"deform", FrameStage::deform, 0});
    const auto ownerOrder = [&modelOrder](const core::SceneEffectInstance& effect) {
        if (!effect.controllerModel.has_value() || !modelOrder)
            return core::ModelExecutionOrder{};
        const auto order = modelOrder(*effect.controllerModel);
        if (!order.has_value())
            throw std::invalid_argument("FX controller model is missing from the scene");
        return *order;
    };
    const auto nameFor = [](const core::SceneEffectInstance& effect) {
        const auto stem = effect.source.stem().string();
        return stem.empty() ? "effect-" + std::to_string(effect.id) : stem;
    };
    for (const auto& effect : effects.deform) {
        const auto name = nameFor(effect);
        if (isEnabled(name))
            result.push_back({name, FrameStage::deform, effect.executionOrder + ownerOrder(effect).deform, effect.id});
    }
    if (effects.renderer.has_value()) {
        const auto name = nameFor(*effects.renderer);
        if (isEnabled(name))
            result.push_back({name, FrameStage::renderer, effects.renderer->executionOrder, effects.renderer->id});
    }
    for (const auto& effect : effects.postprocess) {
        const auto name = nameFor(effect);
        if (isEnabled(name))
            result.push_back({name, FrameStage::postPre, effect.executionOrder + ownerOrder(effect).postprocess,
                              effect.id});
    }
    const auto rank = [](FrameStage stage) {
        switch (stage) {
        case FrameStage::deform:
            return 0;
        case FrameStage::renderer:
            return 1;
        case FrameStage::postPre:
            return 2;
        case FrameStage::tonemap:
            return 3;
        case FrameStage::postPost:
            return 4;
        case FrameStage::present:
            return 5;
        }
        return 1;
    };
    std::stable_sort(result.begin(), result.end(), [&](const auto& left, const auto& right) {
        const auto leftRank = rank(left.stage);
        const auto rightRank = rank(right.stage);
        return leftRank == rightRank ? left.order < right.order : leftRank < rightRank;
    });
    result.push_back({"present", FrameStage::present, 1000});
    return result;
}

} // namespace dayo::fx
