#include "fx/fx_preview_path.hpp"

#include "core/log.hpp"

#include <algorithm>

namespace dayo::fx {

const char* toString(PreviewFxPass pass) noexcept {
    switch (pass) {
    case PreviewFxPass::bg:
        return "BG";
    case PreviewFxPass::mmd:
        return "MMD";
    case PreviewFxPass::gbuffer:
        return "GBuffer";
    case PreviewFxPass::copy:
        return "Copy";
    case PreviewFxPass::denoise:
        return "DENOISE";
    }
    return "BG";
}

FxProgram makePreviewReferenceProgram() {
    FxProgram program;
    program.label = "PreviewReference";
    program.generation = 1;
    program.passes = {
        {"BG", FxOpKind::clear, {}, 1, 1, {{"preview.background", true}}},
        {"MMD", FxOpKind::raster, {}, 1, 1, {{"preview.color", true}}},
        {"GBuffer", FxOpKind::raster, {}, 1, 1, {{"preview.gbuffer", true}}},
        {"Copy", FxOpKind::copy, {}, 1, 1, {{"preview.gbuffer", false}, {"preview.color", true}}},
        {"DENOISE", FxOpKind::compute, {}, 1, 1, {{"preview.color", false}, {"preview.denoised", true}}},
    };
    return program;
}

FxFramePlan buildPreviewReferencePlan(const FxFrameContext& context) {
    return buildPreviewReferencePlan(makePreviewReferenceProgram(), context);
}

FxFramePlan buildPreviewReferencePlan(const FxProgram& program, const FxFrameContext& context) {
    FxCompiler compiler;
    FxFramePlan plan = compiler.plan(program, context);
    // Enforce the canonical 5-pass reference order even if the program was
    // built from a reordered file: BG -> MMD -> GBuffer -> Copy -> DENOISE.
    const auto rank = [](const FxDispatch& dispatch) -> int {
        if (dispatch.name == "BG")
            return 0;
        if (dispatch.name == "MMD")
            return 1;
        if (dispatch.name == "GBuffer")
            return 2;
        if (dispatch.name == "Copy")
            return 3;
        if (dispatch.name == "DENOISE")
            return 4;
        switch (dispatch.kind) {
        case FxOpKind::clear:
            return 0;
        case FxOpKind::raster:
            return 1;
        case FxOpKind::copy:
            return 3;
        case FxOpKind::compute:
            return 4;
        default:
            return 2;
        }
    };
    std::stable_sort(plan.ordered.begin(), plan.ordered.end(),
                     [&](const FxDispatch& left, const FxDispatch& right) { return rank(left) < rank(right); });
    if (plan.ordered.size() != previewFxPassCount())
        dayo::log::debug("Preview reference plan has ", plan.ordered.size(), " passes (canonical is 5)");
    return plan;
}

} // namespace dayo::fx
