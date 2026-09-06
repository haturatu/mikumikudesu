#pragma once

#include "core/effect.hpp"
#include "core/fx/fx_condition.hpp"
#include "core/fx/fx_material.hpp"
#include "core/fx/fx_pass.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dayo::core::fx {

// TASKS.md section 11 FxFrameContext ABI. Field names and widths match the
// spec; camera/lighting payloads ride along once Scene exposes
// CameraData/LightingData on main (omitted here so this header only depends
// on effect.hpp). `currentModel` aliases dayo::core::ModelId (uint64_t).
struct FxFrameContext {
    std::uint64_t frameIndex{};
    std::uint64_t sampleIndex{};

    std::uint32_t renderWidth{};
    std::uint32_t renderHeight{};

    std::uint64_t currentModel{};
    std::uint32_t modelIndex{};

    std::uint64_t vertexCount{};
    std::uint64_t totalMaterial{};

    std::uint32_t cloneCount{1};
    std::uint64_t clonedVertexCount{};
};

// TASKS.md section 5 lifecycle scheduler mapping:
//
//   load effect                              -> OnLoad   (kFxEventLoad)
//   renderer selected / playback restarted   -> OnStart  (kFxEventStart)
//   swapchain / output resolution changed    -> OnResize (kFxEventResize)
//   frame                                    -> Frame    (kFxEventFrame)
//   model set changed                        -> modelChanged
//   material set changed                     -> materialChanged
//
// The scheduler only matches event masks; predicate evaluation stays with
// the runtime (PR1 binds the opaque predicate text to the FxExpr AST).
enum class FxLifecyclePoint { load, start, resize, frame };

[[nodiscard]] FxEventMask fxEventForLifecyclePoint(FxLifecyclePoint point) noexcept;
[[nodiscard]] const char* toString(FxLifecyclePoint point) noexcept;

// TASKS.md section 1: the linker produces an FxProgram from parsed
// documents (EffectGraph today, .fxdayo FxDocument once PR1 lands).
struct FxProgram {
    std::string name;
    std::string categoryName;
    FxCategory category{FxCategory::render};
    MaterialBindingPlan material;
    std::vector<FxPass> passes;
    FxConditionScheduler scheduler;
};

[[nodiscard]] FxProgram linkFxProgram(const EffectGraph& graph, FxCategory category);
[[nodiscard]] FxProgram linkFxProgram(const EffectGraph& graph, std::string_view categoryName);
[[nodiscard]] FxProgram linkFxProgram(const EffectGraph& graph, std::string_view categoryName,
                                      const MaterialTemplate& material, const MaterialInstance* instance = nullptr);

// FxInstance binds a linked program to one frame's context (TASKS.md
// section 1: FxInstance = Scene/model/material/controller bound to reality).
struct FxInstance {
    const FxProgram* program{};
    FxFrameContext context{};

    [[nodiscard]] std::vector<int> activePasses(FxEventMask active) const;
};

// TASKS.md section 6: rasterModelTarget is resolved at schedule time, NOT
// in the executor. The Vulkan executor receives concrete model indices and
// never learns what "self" means. `buffer` intentionally fans out to
// nothing: mesh-buffer targets dispatch by buffer range, not per model.
[[nodiscard]] std::vector<std::uint32_t> expandRasterModelTargets(RasterModelTarget target, std::uint32_t modelCount,
                                                                  std::uint32_t selfIndex) noexcept;

} // namespace dayo::core::fx
