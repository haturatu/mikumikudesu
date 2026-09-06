#include "core/fx_debug.hpp"

#include <sstream>

namespace dayo::core {

FxRuntimeDebugSnapshot FxRuntimeInspector::snapshot(const EffectGraph& graph, std::uint64_t frame) {
    FxRuntimeDebugSnapshot result;
    result.frame = frame;
    result.passCount = static_cast<std::uint32_t>(graph.passes.size());
    result.materialCount = static_cast<std::uint32_t>(graph.textures.size() + graph.buffers.size());
    result.backend = "preview";
    result.passNames.reserve(graph.passes.size());
    for (const auto& pass : graph.passes)
        result.passNames.push_back(pass.name);
    if (graph.passes.empty())
        result.warnings.emplace_back("no passes compiled");
    return result;
}

FxRuntimeDebugSnapshot FxRuntimeInspector::empty(std::uint64_t frame) {
    FxRuntimeDebugSnapshot result;
    result.frame = frame;
    result.warnings.emplace_back("no effect bound");
    return result;
}

std::string FxRuntimeInspector::format(const FxRuntimeDebugSnapshot& snapshot) {
    std::ostringstream line;
    line << "fx frame=" << snapshot.frame << " passes=" << snapshot.passCount
         << " materials=" << snapshot.materialCount << " backend=" << snapshot.backend;
    return line.str();
}

} // namespace dayo::core
