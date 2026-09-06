#include "fx/fx_scheduler.hpp"

#include <algorithm>

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
    switch (entry.category) {
    case FxCategory::renderer:
    case FxCategory::particle:
    case FxCategory::sample:
        return FrameStage::renderer;
    case FxCategory::postprocess:
    case FxCategory::unknown:
        break;
    }
    // postprocess split: explicit tonemap names run at tonemap, low orders
    // run before tonemap, the rest after.
    if (entry.name == "tonemap" || entry.name == "Tonemap" || entry.name == "TONEMAP")
        return FrameStage::tonemap;
    return entry.executionOrder < 100 ? FrameStage::postPre : FrameStage::postPost;
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
        if (entry.category == FxCategory::postprocess)
            posts.push_back(entry);
        else
            renderers.push_back(entry);
    }
    // Active renderer first, then remaining renderer/particle/sample in order.
    std::sort(renderers.begin(), renderers.end(), [](const auto& left, const auto& right) {
        return left.executionOrder < right.executionOrder;
    });
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

    // Stable stage ordering: deform -> renderer -> postPre -> tonemap ->
    // postPost -> present. Within a stage keep executionOrder.
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

} // namespace dayo::fx
