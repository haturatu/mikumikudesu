#include "fx/fx_compiler.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace dayo::fx {

const char* toString(FxOpKind kind) noexcept {
    switch (kind) {
    case FxOpKind::raster:
        return "raster";
    case FxOpKind::postprocess:
        return "postprocess";
    case FxOpKind::compute:
        return "compute";
    case FxOpKind::copy:
        return "copy";
    case FxOpKind::clear:
        return "clear";
    case FxOpKind::mipmap:
        return "mipmap";
    case FxOpKind::raytracing:
        return "raytracing";
    }
    return "raster";
}

FxOpKind fxOpFromPassType(core::EffectPassType type) {
    switch (type) {
    case core::EffectPassType::rasterizer:
        return FxOpKind::raster;
    case core::EffectPassType::postprocess:
        return FxOpKind::postprocess;
    case core::EffectPassType::compute:
        return FxOpKind::compute;
    case core::EffectPassType::raytracing:
        return FxOpKind::raytracing;
    case core::EffectPassType::unknown:
        throw std::runtime_error("unsupported FX pass type: unknown");
    }
    throw std::runtime_error("unsupported FX pass type");
}

core::EffectGraph FxCompiler::parse(const FxSourceDocument& document) const {
    if (document.raw.empty()) {
        throw std::runtime_error("fx source is empty: " + document.path.string());
    }
    // Parse the caller's immutable buffer. In particular, a watcher/editor
    // may have newer text than the path on disk.
    try {
        core::EffectGraph graph = core::loadEffectGraphFromText(document.path, document.raw);
        graph.sourcePath = document.path;
        return graph;
    } catch (const std::exception&) {
        if (!options_.allowSyntheticProgramForTests)
            throw;
        core::EffectGraph graph;
        graph.sourcePath = document.path;
        graph.category = "generic";
        core::EffectPass pass;
        pass.name = document.path.stem().string();
        if (pass.name.empty())
            pass.name = "main";
        pass.type = core::EffectPassType::rasterizer;
        graph.passes.push_back(std::move(pass));
        dayo::log::debug("FxCompiler using explicit test-only synthetic pass for ", document.path.string());
        return graph;
    }
}

core::EffectGraph FxCompiler::link(const core::EffectGraph& graph, std::string* error) const {
    for (const auto& pass : graph.passes) {
        if (pass.name.empty()) {
            if (error != nullptr)
                *error = "fx link: pass with empty name";
            throw std::runtime_error("fx link: pass with empty name");
        }
    }
    return graph;
}

FxProgram FxCompiler::compile(const core::EffectGraph& graph) const {
    FxProgram program;
    program.label = graph.sourcePath.string();
    if (program.label.empty())
        program.label = graph.category.empty() ? "fx" : graph.category;
    program.generation = 1;
    for (const auto& pass : graph.passes) {
        if (pass.type == core::EffectPassType::unknown)
            throw std::runtime_error("unsupported FX pass '" + pass.name + "': unknown type");
        FxDispatch dispatch;
        dispatch.name = pass.name.empty() ? "pass" : pass.name;
        dispatch.kind = fxOpFromPassType(pass.type);
        if (!pass.computeShader.empty())
            dispatch.shader = pass.computeShader;
        else if (!pass.pixelShader.empty())
            dispatch.shader = pass.pixelShader;
        else
            dispatch.shader = pass.vertexShader;
        program.passes.push_back(std::move(dispatch));
    }
    if (program.passes.empty())
        throw std::runtime_error("FX graph contains no passes");
    return program;
}

FxProgram FxCompiler::compileSource(const FxSourceDocument& document) const {
    auto graph = parse(document);
    auto linked = link(graph);
    return compile(linked);
}

FxFramePlan FxCompiler::plan(const FxProgram& program, const FxFrameContext& context) const {
    FxFramePlan framePlan;
    framePlan.ordered = program.passes;
    framePlan.programGeneration = program.generation;
    framePlan.renderWidth = context.renderWidth;
    framePlan.renderHeight = context.renderHeight;
    return framePlan;
}

bool FxCompiler::buildPipelines(const FxProgram& program, std::string* error) const {
    const auto empty = std::ranges::find_if(program.passes, [](const FxDispatch& pass) { return pass.name.empty(); });
    if (empty == program.passes.end())
        return true;
    if (error != nullptr)
        *error = "fx pipeline: dispatch with empty name";
    dayo::log::error("FxCompiler pipeline validation failed: empty dispatch name");
    return false;
}

FxInstance::FxInstance(FxProgram initial, FxCompilerOptions options)
    : active_(std::make_shared<const FxProgram>(std::move(initial))), compiler_(options) {}

std::shared_ptr<const FxProgram> FxInstance::active() const {
    std::scoped_lock lock(mutex_);
    return active_;
}

bool FxInstance::tryHotReload(const FxSourceDocument& document, const FxFrameContext& contextForPlan,
                              std::string* error) {
    // parse -> link -> compile -> plan -> pipeline; swap only on success.
    if (error != nullptr)
        error->clear();
    core::EffectGraph graph;
    try {
        graph = compiler_.parse(document);
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = std::string("fx parse: ") + exception.what();
        dayo::log::warn("FxInstance hot reload parse failed, keeping current: ", exception.what());
        return false;
    }
    try {
        graph = compiler_.link(graph, error);
    } catch (const std::exception& exception) {
        if (error != nullptr && error->empty())
            *error = std::string("fx link: ") + exception.what();
        dayo::log::warn("FxInstance hot reload link failed, keeping current: ", exception.what());
        return false;
    }
    FxProgram candidate;
    try {
        candidate = compiler_.compile(graph);
        candidate.generation = active()->generation + 1;
        // Plan validation before pipeline creation catches size/context errors.
        static_cast<void>(compiler_.plan(candidate, contextForPlan));
        std::string pipelineError;
        if (!compiler_.buildPipelines(candidate, &pipelineError)) {
            if (error != nullptr)
                *error = pipelineError;
            dayo::log::warn("FxInstance hot reload pipeline failed, keeping current: ", pipelineError);
            return false;
        }
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = std::string("fx compile: ") + exception.what();
        dayo::log::warn("FxInstance hot reload compile failed, keeping current: ", exception.what());
        return false;
    }
    {
        std::scoped_lock lock(mutex_);
        if (active_ && active_->generation >= candidate.generation)
            candidate.generation = active_->generation + 1;
        pending_ = std::move(candidate);
    }
    return commitPendingAtFrameBoundary();
}

bool FxInstance::stagePending(const FxSourceDocument& document, std::string* error) {
    try {
        FxProgram candidate = compiler_.compileSource(document);
        std::string pipelineError;
        if (!compiler_.buildPipelines(candidate, &pipelineError)) {
            if (error != nullptr)
                *error = pipelineError;
            return false;
        }
        std::scoped_lock lock(mutex_);
        pending_ = std::move(candidate);
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = exception.what();
        return false;
    }
}

bool FxInstance::commitPendingAtFrameBoundary() {
    std::scoped_lock lock(mutex_);
    if (!pending_)
        return false;
    auto previous = active_;
    auto next = std::make_shared<const FxProgram>(std::move(*pending_));
    pending_.reset();
    active_ = std::move(next);
    if (previous) {
        retired_.push_back({std::move(previous), nextTimelineValue_});
        dayo::log::info("FxInstance swapped program generation ", active_->generation, " at frame boundary");
    }
    return true;
}

bool FxInstance::hasPending() const noexcept {
    std::scoped_lock lock(mutex_);
    return pending_.has_value();
}

void FxInstance::retireCompleted(std::uint64_t timelineCompleted) noexcept {
    std::scoped_lock lock(mutex_);
    std::vector<Retired> alive;
    alive.reserve(retired_.size());
    for (auto& entry : retired_) {
        if (entry.retireTimeline > timelineCompleted)
            alive.push_back(std::move(entry));
    }
    retired_.swap(alive);
}

std::size_t FxInstance::retiredCount() const noexcept {
    std::scoped_lock lock(mutex_);
    return retired_.size();
}

} // namespace dayo::fx
