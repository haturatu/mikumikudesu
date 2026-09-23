#pragma once

#include "core/effect.hpp"
#include "core/fx/fx_material.hpp"
#include "core/fx/fx_pass.hpp"
#include "fx/fx_document.hpp"
#include "fx/fx_frame.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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
    oidn,
};

[[nodiscard]] const char* toString(FxOpKind kind) noexcept;
[[nodiscard]] FxOpKind fxOpFromPassType(core::EffectPassType type);

struct FxCompilerOptions {
    // Test-only compatibility for callers that intentionally use a synthetic
    // document without YRZFX/HLSL sections. Production parsing remains strict.
    bool allowSyntheticProgramForTests{false};
};

enum class FxResourceRole : std::uint8_t {
    sampled,
    storage,
    colorAttachment,
    depthAttachment,
};

struct FxRasterDispatch {
    std::string vertexShader;
    std::string pixelShader;
    core::EffectGraphicsState graphics;
    std::vector<core::EffectAttachment> colorAttachments;
    std::optional<core::EffectAttachment> depthAttachment;
    core::EffectRasterSource rasterSource{core::EffectRasterSource::scene};
    core::EffectVertexLayout vertexLayout;
    std::string vertexBuffer;
    std::string indexBuffer;
};

struct FxPostProcessDispatch {
    std::string pixelShader;
    std::vector<core::EffectAttachment> colorAttachments;
};

struct FxComputeDispatch {
    std::string computeShader;
};

struct FxRayTracingDispatch {
    std::string rayGenerationShader;
    std::vector<std::string> missShaders;
    std::vector<core::fx::FxRayTracingHitGroup> hitGroups;
    std::vector<std::string> callableShaders;
    std::uint32_t maxPayloadSize{};
    std::uint32_t maxAttributeSize{};
    std::uint32_t maxRecursionDepth{1};
};

struct FxOidnDispatch {
    // input is the upstream beauty/color resource. Albedo and normal are
    // optional auxiliary resources accepted by the OIDN RT filter.
    std::string input;
    std::string albedo;
    std::string normal;
    std::string output;
};

struct FxUtilityDispatch {};

using FxExecutable = std::variant<FxRasterDispatch, FxPostProcessDispatch, FxComputeDispatch, FxRayTracingDispatch,
                                  FxOidnDispatch, FxUtilityDispatch>;

struct FxDispatch {
    struct ResourceUse {
        std::string name;
        bool write{};
        FxResourceRole role{FxResourceRole::sampled};
    };
    FxDispatch() = default;
    FxDispatch(std::string passName, FxOpKind passKind, std::string passShader, std::uint32_t widthNumerator,
               std::uint32_t widthDenominator, std::vector<ResourceUse> passResources,
               std::vector<std::string> passConditions, FxExecutable passExecutable,
               std::vector<std::string> passMacros)
        : name(std::move(passName)), kind(passKind), shader(std::move(passShader)), widthRatioNumerator(widthNumerator),
          widthRatioDenominator(widthDenominator), resources(std::move(passResources)),
          conditions(std::move(passConditions)), executable(std::move(passExecutable)), macros(std::move(passMacros)) {}
    std::string name;
    FxOpKind kind{FxOpKind::raster};
    std::string shader;
    std::uint32_t widthRatioNumerator{1};
    std::uint32_t widthRatioDenominator{1};
    std::vector<ResourceUse> resources;
    // Conditions remain attached to the dispatch until the frame executor
    // evaluates them; compiling them away would make conditional passes run
    // unconditionally when the plan is shared with another scheduler.
    std::vector<std::string> conditions;
    // Typed executable payload. The legacy fields above remain source
    // compatible for Preview callers, while native backends consume this
    // lossless variant.
    FxExecutable executable{FxRasterDispatch{}};
    std::vector<std::string> macros;
    core::fx::FxCategory category{core::fx::FxCategory::render};
    std::array<std::uint32_t, 3> numThreads{};
    core::EffectSize outputSize;
    float outputWidthRatio{1.0F};
    float outputHeightRatio{1.0F};
    core::EffectFunctionalPassKind functionalKind{core::EffectFunctionalPassKind::none};
    core::EffectFunctionalDispatch functional;
};

enum class FxDescriptorClass : std::uint8_t {
    sampledImage,
    storageImage,
    storageBuffer,
    sampler,
    accelerationStructure,
};

[[nodiscard]] constexpr std::uint32_t fxDescriptorBindingBase(FxDescriptorClass descriptorClass) noexcept {
    switch (descriptorClass) {
    case FxDescriptorClass::storageImage:
        return 0;
    case FxDescriptorClass::sampledImage:
    case FxDescriptorClass::storageBuffer:
    case FxDescriptorClass::accelerationStructure:
        return 16;
    case FxDescriptorClass::sampler:
        return 32;
    }
    return 0;
}

[[nodiscard]] constexpr char fxDescriptorRegister(FxDescriptorClass descriptorClass, bool writable = false) noexcept {
    switch (descriptorClass) {
    case FxDescriptorClass::storageImage:
        return 'u';
    case FxDescriptorClass::storageBuffer:
        return writable ? 'u' : 't';
    case FxDescriptorClass::sampledImage:
    case FxDescriptorClass::accelerationStructure:
        return 't';
    case FxDescriptorClass::sampler:
        return 's';
    }
    return 't';
}

[[nodiscard]] constexpr std::uint32_t fxDescriptorBindingBaseForUse(FxDescriptorClass descriptorClass,
                                                                    bool writable) noexcept {
    return descriptorClass == FxDescriptorClass::storageBuffer && writable ? 0U
                                                                           : fxDescriptorBindingBase(descriptorClass);
}

struct FxLogicalBinding {
    std::string resource;
    FxDescriptorClass descriptorClass{FxDescriptorClass::sampledImage};
    std::uint32_t set{};
    // `binding` is the physical Vulkan binding. Shader generation derives the
    // HLSL register index from the descriptor class and this shared value.
    std::uint32_t binding{};
    std::uint32_t count{1};
    bool writable{};
};

struct FxMaterialDescriptorPlan {
    FxLogicalBinding materialIndices;
    FxLogicalBinding textureIndices2D;
    FxLogicalBinding textureIndices3D;
    FxLogicalBinding values;
    FxLogicalBinding textures2D;
    FxLogicalBinding textures3D;
};

struct FxPassBindingPlan {
    std::vector<FxLogicalBinding> bindings;
    std::optional<FxMaterialDescriptorPlan> material;

    [[nodiscard]] const FxLogicalBinding* find(std::string_view resource) const noexcept {
        for (const auto& binding : bindings)
            if (binding.resource == resource)
                return &binding;
        return nullptr;
    }
};

struct FxProgram;
[[nodiscard]] FxPassBindingPlan planPassBindings(const FxProgram& program, const FxDispatch& dispatch,
                                                 std::uint32_t resourceSet);

struct FxProgram {
    std::string label;
    std::vector<FxDispatch> passes;
    // Resource declarations are part of the compiled program. Keeping them
    // here prevents native backends from having to reconstruct typed image,
    // buffer, and sampler metadata from a source graph that may already have
    // been replaced by hot reload.
    std::vector<core::EffectTexture> textures;
    std::vector<core::EffectTexture> textures3D;
    std::vector<core::EffectBuffer> buffers;
    std::vector<core::EffectSampler> samplers;
    std::vector<core::EffectController> controllers;
    std::uint32_t meshCloneCount{1};
    std::uint64_t generation{};
    std::uint64_t sourceVersion{};
    std::filesystem::path sourcePath;
    std::optional<core::EffectMaterialDescriptor> materialDescriptor;
    std::optional<core::fx::MaterialTemplateSchema> materialSchema;
    std::vector<std::string> memos;
    std::uint32_t globalVarSize{1024};
    bool globalVarSizeSpecified{};
    std::string rawYrzfx;
    std::string hlslPrefix;
    std::string generatedCode;
    std::string hlsl;
    core::fx::FxCategory category{core::fx::FxCategory::render};
};

struct FxExtent3D {
    std::uint32_t width{};
    std::uint32_t height{1};
    std::uint32_t depth{1};
    std::uint32_t dimension{1};
};

struct FxResolvedRasterTarget {
    std::vector<std::string> colors;
    std::optional<std::string> depth;
    std::optional<std::string> vertexBuffer;
    std::optional<std::string> indexBuffer;
    std::uint32_t vertexCount{};
    std::uint32_t indexCount{};
};

struct FxResolvedPass {
    std::size_t sourceIndex{};
    FxExtent3D outputExtent{};
    FxExtent3D dispatchGroups{1, 1, 1};
    std::array<std::uint32_t, 3> numThreads{};
    FxPassBindingPlan bindings;
    bool enabled{true};
    std::optional<FxResolvedRasterTarget> raster;
};

struct FxRequiredFeatures {
    bool descriptorIndexing{};
    bool accelerationStructure{};
    bool rayQuery{};
    bool rayTracingPipeline{};
    bool fragmentShaderBarycentric{};
};

[[nodiscard]] FxRequiredFeatures requiredFeatures(const FxProgram& program) noexcept;

struct FxFramePlan {
    std::vector<FxDispatch> ordered;
    std::vector<FxResolvedPass> resolved;
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
