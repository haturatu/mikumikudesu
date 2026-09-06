#include "core/fx/fx_link.hpp"

#include "core/log.hpp"

#include <utility>

namespace dayo::core::fx {

FxEventMask fxEventForLifecyclePoint(FxLifecyclePoint point) noexcept {
    switch (point) {
    case FxLifecyclePoint::load:
        return kFxEventLoad;
    case FxLifecyclePoint::start:
        return kFxEventStart;
    case FxLifecyclePoint::resize:
        return kFxEventResize;
    case FxLifecyclePoint::frame:
        return kFxEventFrame;
    }
    return kFxEventNone;
}

const char* toString(FxLifecyclePoint point) noexcept {
    switch (point) {
    case FxLifecyclePoint::load:
        return "load";
    case FxLifecyclePoint::start:
        return "start";
    case FxLifecyclePoint::resize:
        return "resize";
    case FxLifecyclePoint::frame:
        return "frame";
    }
    return "frame";
}

FxProgram linkFxProgram(const EffectGraph& graph, FxCategory category) {
    FxProgram program;
    program.name = graph.category;
    program.categoryName = std::string(toString(category));
    program.category = category;
    program.passes.reserve(graph.passes.size());
    for (std::size_t i = 0; i < graph.passes.size(); ++i) {
        FxPass pass = fxPassFromEffectPass(graph.passes[i], category);
        FxConditionProgram combined;
        for (const auto& condition : pass.conditions) {
            const FxConditionProgram compiled = compileFxCondition(condition);
            combined.events |= compiled.events;
            if (!compiled.predicate.empty()) {
                if (!combined.predicate.empty())
                    combined.predicate += " && ";
                combined.predicate += compiled.predicate;
            }
        }
        pass.eventMask = combined.events;
        program.scheduler.add(std::move(combined), static_cast<int>(i), pass.name);
        program.passes.push_back(std::move(pass));
    }
    log::debug("fx link: program '", program.name, "' linked ", program.passes.size(), " passes");
    return program;
}

FxProgram linkFxProgram(const EffectGraph& graph, std::string_view categoryName) {
    return linkFxProgram(graph, fxCategoryFromString(categoryName));
}

FxProgram linkFxProgram(const EffectGraph& graph, std::string_view categoryName, const MaterialTemplate& material,
                        const MaterialInstance* instance) {
    FxProgram program = linkFxProgram(graph, fxCategoryFromString(categoryName));
    program.material = linkMaterial(material, instance);
    return program;
}

std::vector<int> FxInstance::activePasses(FxEventMask active) const {
    if (program == nullptr)
        return {};
    return program->scheduler.activePasses(active);
}

std::vector<std::uint32_t> expandRasterModelTargets(RasterModelTarget target, std::uint32_t modelCount,
                                                    std::uint32_t selfIndex) noexcept {
    std::vector<std::uint32_t> out;
    try {
        switch (target) {
        case RasterModelTarget::all:
            for (std::uint32_t i = 0; i < modelCount; ++i)
                out.push_back(i);
            break;
        case RasterModelTarget::self:
            if (selfIndex < modelCount)
                out.push_back(selfIndex);
            break;
        case RasterModelTarget::other:
            for (std::uint32_t i = 0; i < modelCount; ++i) {
                if (i != selfIndex)
                    out.push_back(i);
            }
            break;
        case RasterModelTarget::buffer:
            break;
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

} // namespace dayo::core::fx
