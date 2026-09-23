#pragma once

#include "core/effect.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dayo::core {

struct FxDebugResourceSnapshot {
    std::string name;
    std::string kind;
    std::string format;
    std::string view;
    std::string shared;
    std::string filename;
    std::string type;
    std::string sizeBase;
    std::vector<std::string> conditions;
    std::uint32_t width{};
    std::uint32_t height{1};
    std::uint32_t depth{1};
    std::uint32_t elementSize{};
    bool absoluteSize{};
    bool mipmapped{};
};

struct FxDebugResourceAccess {
    std::string name;
    bool write{};
};

struct FxDebugPassSnapshot {
    std::string name;
    std::string type;
    std::string functionalKind;
    std::vector<FxDebugResourceAccess> resources;
    std::vector<std::string> conditions;
};

struct FxDebugControllerSnapshot {
    std::string name;
    std::string controller;
    std::string item;
    std::string type;
    std::string description;
    std::optional<EffectSlider> slider;
};

// Inspector-only snapshot of FX runtime state. The inspector receives const
// references (EffectGraph/Scene summaries) and never raw Vulkan handles.
struct FxRuntimeDebugSnapshot {
    std::uint64_t frame{};
    std::uint32_t passCount{};
    std::uint32_t resourceCount{};
    std::uint32_t materialCount{};
    std::string backend{"preview"};
    std::vector<std::string> passNames;
    std::vector<std::string> warnings;
    std::vector<FxDebugResourceSnapshot> resources;
    std::vector<FxDebugPassSnapshot> passes;
    std::vector<FxDebugControllerSnapshot> controllers;
    std::vector<std::string> memos;
    std::uint32_t globalVarSize{};
};

class FxRuntimeInspector {
  public:
    [[nodiscard]] static FxRuntimeDebugSnapshot snapshot(const EffectGraph& graph, std::uint64_t frame);
    [[nodiscard]] static FxRuntimeDebugSnapshot empty(std::uint64_t frame);
    [[nodiscard]] static std::string format(const FxRuntimeDebugSnapshot& snapshot);
};

} // namespace dayo::core
