#pragma once

#include "core/effect.hpp"
#include "fx/fx_document.hpp"
#include "fx/fx_frame.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace dayo::fx {

// Extended dispatch kinds. core::EffectPassType covers file formats;
// the executor also needs explicit copy/clear/mipmap control passes
// that never appear as raytracing. Raytracing stays in the plan so the
// executor can fail explicitly instead of silently skipping.
enum class FxOpKind : std::uint8_t {
    raster,
    postprocess,
    compute,
    copy,
    clear,
    mipmap,
    raytracing,
};

[[nodiscard]] const char* toString(FxOpKind kind) noexcept;
[[nodiscard]] FxOpKind fxOpFromPassType(core::EffectPassType type);

struct FxCompilerOptions {
    // Test-only compatibility for callers that intentionally use a synthetic
    // document without YRZFX/HLSL sections. Production parsing remains strict.
    bool allowSyntheticProgramForTests{false};
};

struct FxDispatch {
    std::string name;
    FxOpKind kind{FxOpKind::raster};
    std::string shader;
    std::uint32_t widthRatioNumerator{1};
    std::uint32_t widthRatioDenominator{1};
    struct ResourceUse {
        std::string name;
        bool write{};
    };
    std::vector<ResourceUse> resources;
    // Conditions remain attached to the dispatch until the frame executor
    // evaluates them; compiling them away would make conditional passes run
    // unconditionally when the plan is shared with another scheduler.
    std::vector<std::string> conditions;
};

struct FxProgram {
    std::string label;
    std::vector<FxDispatch> passes;
    std::uint64_t generation{};
    std::uint64_t sourceVersion{};
};

struct FxFramePlan {
    std::vector<FxDispatch> ordered;
    std::uint64_t programGeneration{};
    std::uint32_t renderWidth{};
    std::uint32_t renderHeight{};
};

// parse -> link -> compile -> plan -> pipeline decomposed for hot reload.
// Each stage is pure; only the frame-boundary swap mutates live state.
class FxCompiler {
  public:
    explicit FxCompiler(FxCompilerOptions options = {}) : options_(options) {}

    // Parse: raw document -> EffectGraph. Throws on missing markers.
    [[nodiscard]] core::EffectGraph parse(const FxSourceDocument& document) const;
    // Link: resolve cross-file references (skeleton: validate pass names).
    [[nodiscard]] core::EffectGraph link(const core::EffectGraph& graph, std::string* error = nullptr) const;
    // Compile: EffectGraph -> FxProgram (generic, no Subayai/BDPT hardcode).
    [[nodiscard]] FxProgram compile(const core::EffectGraph& graph) const;
    [[nodiscard]] FxProgram compileSource(const FxSourceDocument& document) const;
    // Plan: FxProgram + frame context -> ordered per-frame dispatches.
    [[nodiscard]] FxFramePlan plan(const FxProgram& program, const FxFrameContext& context) const;
    // Pipeline: validate that every dispatch has backend support short of
    // raytracing (which the executor rejects explicitly at record time).
    bool buildPipelines(const FxProgram& program, std::string* error = nullptr) const;

  private:
    FxCompilerOptions options_;
};

// Per-effect live slot. Hot reload stages a candidate off-thread and swaps
// only on success at a frame boundary. Failures keep the current program.
// Retired programs stay alive until the timeline semaphore passes so
// in-flight GPU work never observes a use-after-free (API skeleton).
class FxInstance {
  public:
    explicit FxInstance(FxProgram initial, FxCompilerOptions options = {});

    [[nodiscard]] std::shared_ptr<const FxProgram> active() const;
    // Full hot-reload pipeline: parse -> link -> compile -> plan -> pipeline.
    // Successful candidates are staged; the render thread must call
    // commitPendingAtFrameBoundary() to mutate active().
    bool tryHotReload(const FxSourceDocument& document, const FxFrameContext& contextForPlan,
                      std::string* error = nullptr);
    // Stage without swapping (for callers that drive frame boundaries).
    bool stagePending(const FxSourceDocument& document, std::string* error = nullptr);
    // Atomic swap staged -> active. Must be called at a frame boundary.
    bool commitPendingAtFrameBoundary();
    [[nodiscard]] bool hasPending() const noexcept;
    void retireCompleted(std::uint64_t timelineCompleted) noexcept;
    [[nodiscard]] std::size_t retiredCount() const noexcept;
    // Timeline value the next commit will retire the previous program at.
    void setNextTimelineValue(std::uint64_t value) noexcept {
        nextTimelineValue_ = value;
    }

  private:
    mutable std::mutex mutex_;
    std::shared_ptr<const FxProgram> active_;
    std::optional<FxProgram> pending_;
    struct Retired {
        std::shared_ptr<const FxProgram> program;
        std::uint64_t retireTimeline{};
    };
    std::vector<Retired> retired_;
    std::uint64_t nextTimelineValue_{1};
    std::uint64_t nextGeneration_{1};
    std::uint64_t newestSourceVersion_{};
    bool hasSourceVersion_{false};
    std::uint64_t requestSequence_{1};
    std::uint64_t newestRequestSequence_{};
    FxCompiler compiler_;

    [[nodiscard]] std::uint64_t beginReloadRequest();
};

} // namespace dayo::fx
