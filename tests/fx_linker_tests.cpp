#include "core/fx/fx_condition.hpp"
#include "core/fx/fx_material.hpp"
#include "core/fx/fx_pass.hpp"

#include <iostream>
#include <string_view>

namespace {

bool check(bool value, std::string_view message) {
    if (!value)
        std::cerr << "FAIL: " << message << '\n';
    return value;
}

dayo::core::fx::MaterialTemplate makeAliasTemplate() {
    using namespace dayo::core::fx;
    MaterialTemplate templ;
    templ.name = "alias";
    templ.defaults.set("roughness", 0.5F);
    templ.resources = {
        {.id = "albedo", .shared = true},
        {.id = "albedoCopy", .ref = "albedo"},
        {.id = "chain", .ref = "albedoCopy"},
        {.id = "baseA", .shareTags = {"base"}},
        {.id = "baseB", .shareTags = {"base"}},
        {.id = "local", .shareTags = {}},
    };
    return templ;
}

} // namespace

int main() {
    using namespace dayo::core::fx;
    using dayo::core::EffectPass;
    using dayo::core::EffectPassType;
    bool ok = true;

    // Alias folding: shared / ref / shareTags collapse to canonical ids.
    {
        const auto templ = makeAliasTemplate();
        const auto layout = linkMaterialLayout(templ, nullptr);
        const auto& table = layout.localToCanonical;
        ok &= check(table.at("albedo") == "shared:albedo", "shared folds to shared:albedo");
        ok &= check(table.at("albedoCopy") == "shared:albedo", "ref folds to shared target");
        ok &= check(table.at("chain") == "shared:albedo", "chained ref folds transitively");
        ok &= check(table.at("baseA") == "tag:base" && table.at("baseB") == "tag:base",
                    "shareTags fold to tag:base");
        ok &= check(table.at("local") == "local", "concrete id keeps its name");
        ok &= check(layout.slotForLocal("chain") == layout.slotForCanonical("shared:albedo"),
                    "aliased locals share one _R slot");
    }

    // UniqueTextures: canonical path+format+colorspace+mip dedup.
    {
        MaterialTemplate templ;
        templ.name = "tex";
        templ.resources = {
            {.id = "a",
             .texture = {.path = "tex\\diffuse.png", .format = "RGBA8", .colorspace = "sRGB", .mipPolicy = "auto"},
             .hasTexture = true},
            {.id = "b",
             .texture = {.path = "tex/diffuse.png", .format = "rgba8", .colorspace = "srgb", .mipPolicy = "AUTO"},
             .hasTexture = true},
            {.id = "c",
             .texture = {.path = "tex/other.png", .format = "rgba8", .colorspace = "srgb", .mipPolicy = "auto"},
             .hasTexture = true},
            {.id = "d",
             .texture = {.path = "tex/diffuse.png", .format = "rgba8", .colorspace = "linear", .mipPolicy = "auto"},
             .hasTexture = true},
        };
        const auto layout = linkMaterialLayout(templ, nullptr);
        ok &= check(layout.uniqueTextures.size() == 3, "unique textures dedup case/separator variants");
        const auto keyA = makeTextureKey(templ.resources[0].texture);
        const auto keyB = makeTextureKey(templ.resources[1].texture);
        ok &= check(keyA == keyB, "texture keys normalize path separators and case");
        ok &= check(makeTextureKey(templ.resources[0].texture) != makeTextureKey(templ.resources[2].texture),
                    "different paths stay distinct");
        ok &= check(makeTextureKey(templ.resources[0].texture) != makeTextureKey(templ.resources[3].texture),
                    "different colorspaces stay distinct");
    }

    // _R determinism golden: same input set in any order -> same slots.
    {
        auto shuffled = [](bool reversed) {
            MaterialTemplate templ;
            templ.name = "slots";
            MaterialResourceDecl zebra{.id = "zebra"};
            MaterialResourceDecl apple{.id = "apple", .shared = true};
            MaterialResourceDecl mango{.id = "mango", .shareTags = {"group"}};
            templ.resources = reversed ? std::vector<MaterialResourceDecl>{zebra, mango, apple}
                                       : std::vector<MaterialResourceDecl>{apple, mango, zebra};
            return linkMaterialLayout(templ, nullptr);
        };
        const auto forward = shuffled(false);
        const auto reversed = shuffled(true);
        ok &= check(forward.canonicalResources.size() == 3, "_R golden sees three canonicals");
        ok &= check(forward.canonicalResources == reversed.canonicalResources, "_R canonical order is stable");
        ok &= check(forward.canonicalToSlot == reversed.canonicalToSlot, "_R slots ignore declaration order");
        // Golden expectation: sorted canonicals get _R0.._R2.
        ok &= check(forward.canonicalResources[0] == "shared:apple", "_R golden canonical[0]");
        ok &= check(forward.canonicalResources[1] == "tag:group", "_R golden canonical[1]");
        ok &= check(forward.canonicalResources[2] == "zebra", "_R golden canonical[2]");
        ok &= check(forward.slotForCanonical("shared:apple") == "_R0", "_R golden slot 0");
        ok &= check(forward.slotForCanonical("tag:group") == "_R1", "_R golden slot 1");
        ok &= check(forward.slotForCanonical("zebra") == "_R2", "_R golden slot 2");
    }

    // Material binding plan overlays instance overrides on template defaults.
    {
        MaterialTemplate templ;
        templ.name = "plan";
        templ.defaults.set("roughness", 0.5F);
        templ.resources = {{.id = "albedo", .shared = true}};
        MaterialInstance instance;
        instance.templateName = "plan";
        instance.overrides.set("roughness", 0.25F);
        const auto plan = linkMaterial(templ, &instance);
        const auto* value = plan.resolvedParameters.find("roughness");
        ok &= check(value != nullptr && std::get<float>(*value) == 0.25F, "binding plan applies overrides");
        ok &= check(plan.slotFor("albedo") != nullptr, "binding plan exposes _R slot");
    }

    // Condition classification: strings compile to event masks + predicates.
    {
        const auto load = compileFxCondition("on load");
        ok &= check(load.events == kFxEventLoad && load.predicate.empty(), "condition 'on load'");
        const auto multi = compileFxCondition("load, frame if time > 0");
        ok &= check(multi.events == (kFxEventLoad | kFxEventFrame), "condition multi-event mask");
        ok &= check(multi.predicate == "time > 0", "condition predicate kept");
        const auto plus = compileFxCondition("start+resize");
        ok &= check(plus.events == (kFxEventStart | kFxEventResize), "condition plus-separated events");
        const auto models = compileFxCondition("on modelChanged, materialChanged");
        ok &= check(models.events == (kFxEventModelChanged | kFxEventMaterialChanged),
                    "condition model/material events");
        ok &= check(fxEventFromName("model_changed") == kFxEventModelChanged, "condition snake_case alias");
        ok &= check(fxEventFromName("MATERIAL-CHANGED") == kFxEventMaterialChanged, "condition kebab-case alias");
        ok &= check(toStringMask(kFxEventLoad | kFxEventFrame) == "load,frame", "condition mask to string");
        const FxConditionProgram unconditional;
        ok &= check(fxConditionMatches(unconditional, kFxEventFrame), "empty mask matches everything");
        ok &= check(fxConditionMatches(load, kFxEventLoad) && !fxConditionMatches(load, kFxEventFrame),
                    "condition matches only its events");
        bool threw = false;
        try {
            static_cast<void>(compileFxCondition("on warp"));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        ok &= check(threw, "condition rejects unknown events");
    }

    // Condition scheduler skeleton.
    {
        FxConditionScheduler scheduler;
        scheduler.add(compileFxCondition("on load"), 0, "init");
        scheduler.add(compileFxCondition("on frame"), 1, "tick");
        scheduler.add(FxConditionProgram{}, 2, "always");
        const auto onLoad = scheduler.activePasses(kFxEventLoad);
        ok &= check(onLoad.size() == 2 && onLoad[0] == 0 && onLoad[1] == 2, "scheduler serves load set");
        const auto onFrame = scheduler.activePasses(kFxEventFrame);
        ok &= check(onFrame.size() == 2 && onFrame[0] == 1 && onFrame[1] == 2, "scheduler serves frame set");
    }

    // Category/type separation: FxCategory is orthogonal to FxPassOp.
    {
        ok &= check(fxCategoryFromString("deform") == FxCategory::deform, "category deform");
        ok &= check(fxCategoryFromString("render") == FxCategory::render, "category render");
        ok &= check(fxCategoryFromString("postprocess") == FxCategory::postprocess, "category postprocess");
        ok &= check(std::string(toString(FxCategory::deform)) == "deform", "category to string");
        ok &= check(toEffectPassType(FxPassOp{FxRasterOp{}}) == EffectPassType::rasterizer, "raster maps to rasterizer");
        ok &= check(toEffectPassType(FxPassOp{FxPostProcessOp{}}) == EffectPassType::postprocess,
                    "postprocess op maps to postprocess");
        ok &= check(toEffectPassType(FxPassOp{FxComputeOp{}}) == EffectPassType::compute, "compute maps to compute");
        ok &= check(toEffectPassType(FxPassOp{FxRayTracingOp{}}) == EffectPassType::raytracing,
                    "raytracing maps to raytracing");
        ok &= check(toEffectPassType(FxPassOp{FxCopyOp{}}) == EffectPassType::unknown, "copy has no legacy type");
        ok &= check(toEffectPassType(FxPassOp{FxClearRtvOp{}}) == EffectPassType::unknown, "clearRtv is utility");
        ok &= check(toEffectPassType(FxPassOp{FxClearUavOp{}}) == EffectPassType::unknown, "clearUav is utility");
        ok &= check(toEffectPassType(FxPassOp{FxMipmapGenOp{}}) == EffectPassType::unknown, "mipmapGen is utility");
        ok &= check(toEffectPassType(FxPassOp{FxOidnOp{}}) == EffectPassType::unknown, "oidn is utility");
        ok &= check(std::string(fxPassOpTypeName(FxPassOp{FxCopyOp{}})) == "copy", "op type name copy");
        ok &= check(defaultCategoryForOp(FxPassOp{FxPostProcessOp{}}) == FxCategory::postprocess,
                    "postprocess op defaults to postprocess");
        ok &= check(defaultCategoryForOp(FxPassOp{FxRasterOp{}}) == FxCategory::render, "raster defaults to render");
        // Same op type can live in different categories (deform vs render).
        const FxPass deformPass{.name = "skin", .category = FxCategory::deform, .op = FxPassOp{FxComputeOp{}}};
        const FxPass renderPass{.name = "skin", .category = FxCategory::render, .op = FxPassOp{FxComputeOp{}}};
        ok &= check(toEffectPassType(deformPass.op) == toEffectPassType(renderPass.op) &&
                        deformPass.category != renderPass.category,
                    "category and op type vary independently");
    }

    // RasterModelTarget semantic resolution.
    {
        ok &= check(resolveRasterModelTarget("all") == RasterModelTarget::all, "target all");
        ok &= check(resolveRasterModelTarget("SELF") == RasterModelTarget::self, "target self case-insensitive");
        ok &= check(resolveRasterModelTarget("other") == RasterModelTarget::other, "target other");
        ok &= check(resolveRasterModelTarget("buffer") == RasterModelTarget::buffer, "target buffer");
        ok &= check(std::string(toString(RasterModelTarget::other)) == "other", "target to string");
        bool threw = false;
        try {
            static_cast<void>(resolveRasterModelTarget("everything"));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        ok &= check(threw, "target rejects unknown semantics");
    }

    // EffectPass interop both directions.
    {
        EffectPass legacy;
        legacy.name = "main";
        legacy.type = EffectPassType::rasterizer;
        legacy.vertexShader = "vs";
        legacy.pixelShader = "ps";
        legacy.renderTargets = {{.name = "color", .clear = false}};
        legacy.depth = {.name = "depth", .clear = true};
        const auto converted = fxPassFromEffectPass(legacy, FxCategory::render);
        ok &= check(std::holds_alternative<FxRasterOp>(converted.op), "legacy raster becomes FxRasterOp");
        ok &= check(converted.category == FxCategory::render, "explicit category survives conversion");
        const auto roundTrip = effectPassFromFxPass(converted);
        ok &= check(roundTrip.type == EffectPassType::rasterizer && roundTrip.vertexShader == "vs",
                    "FxRasterOp round trips to EffectPass");
        const EffectPass computeLegacy{
            .name = "cs", .type = EffectPassType::compute, .computeShader = "cs_main"};
        const auto computeConverted = fxPassFromEffectPass(computeLegacy, "render");
        ok &= check(std::holds_alternative<FxComputeOp>(computeConverted.op), "legacy compute converts");
    }

    if (!ok)
        std::cerr << "fx_linker_tests: FAILED\n";
    else
        std::cout << "fx_linker_tests: all checks passed\n";
    return ok ? 0 : 1;
}
