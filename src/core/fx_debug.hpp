#pragma once

#include "core/effect.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace dayo::core {

// Inspector-only snapshot of FX runtime state. The inspector receives const
// references (EffectGraph/Scene summaries) and never raw Vulkan handles.
struct FxRuntimeDebugSnapshot {
    std::uint64_t frame{};
    std::uint32_t passCount{};
    std::uint32_t materialCount{};
    std::string backend{"preview"};
    std::vector<std::string> passNames;
    std::vector<std::string> warnings;
};

class FxRuntimeInspector {
  public:
    [[nodiscard]] static FxRuntimeDebugSnapshot snapshot(const EffectGraph& graph, std::uint64_t frame);
    [[nodiscard]] static FxRuntimeDebugSnapshot empty(std::uint64_t frame);
    [[nodiscard]] static std::string format(const FxRuntimeDebugSnapshot& snapshot);
};

} // namespace dayo::core
