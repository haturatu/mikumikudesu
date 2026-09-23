#include "core/fx/fx_condition.hpp"
#include "core/fx/fx_material.hpp"
#include "core/fx/fx_pass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

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
        {.id = "albedo", .shared = true},       {.id = "albedoCopy", .ref = "albedo"},
        {.id = "chain", .ref = "albedoCopy"},   {.id = "baseA", .shareTags = {"base"}},
        {.id = "baseB", .shareTags = {"base"}}, {.id = "local", .shareTags = {}},
    };
    return templ;
}

} // namespace

int main() {
    using namespace dayo::core::fx;
    using dayo::core::EffectPass;
    using dayo::core::EffectPassType;
    bool ok = true;

    // Upstream MatDesc declarations are retained in source order so generated
    // HLSL and subsequent GPU packing share one ABI schema.
    {
        const std::string source = "\xEF\xBB\xBF"
                                   "i.1 : Category\n"
                                   "f.3 : Emission\n"
                                   "i.1 : Mode\n"
                                   "_T0m : NormalMap\n"
                                   "_V1 : VolumeMap\n"
                                   "_TNormalMap : \"template-normal.png\"\n"
                                   "Category : glass\n"
                                   "Mode : on\n"
                                   "_E Category : default=0, glass=1\n"
                                   "_E : Mode : off, on\n";
        const auto schema = parseMaterialTemplateSchema(source, "Subayai");
        ok &= check(schema.name == "Subayai" && schema.sourceText == source,
                    "material schema preserves its name and complete source document");
        ok &= check(schema.fields.size() == 3 && schema.fields[0].name == "Category" &&
                        schema.fields[0].type == MaterialFieldType::signedInteger && schema.fields[0].components == 1 &&
                        schema.fields[1].name == "Emission" &&
                        schema.fields[1].type == MaterialFieldType::floatingPoint && schema.fields[1].components == 3 &&
                        schema.fields[2].name == "Mode",
                    "material value fields preserve declaration order, scalar type, and vector width");
        ok &= check(schema.textures.size() == 2 && schema.textures[0].name == "NormalMap" &&
                        schema.textures[0].index == 0 && schema.textures[0].mipmapped &&
                        schema.textures[0].dimension == MaterialTextureDimension::twoD &&
                        schema.textures[1].name == "VolumeMap" && schema.textures[1].index == 1 &&
                        schema.textures[1].dimension == MaterialTextureDimension::threeD,
                    "material texture declarations retain source indices, mip policy, and dimension");
        ok &= check(schema.templateTextureAssignments.size() == 1 &&
                        schema.templateTextureAssignments[0].field == "NormalMap" &&
                        schema.templateTextureAssignments[0].path == "template-normal.png" &&
                        schema.templateTextureAssignments[0].index == 0,
                    "template texture default resolves to its declared indexed texture");
        const auto* category = schema.defaults.find("Category");
        const auto* mode = schema.defaults.find("Mode");
        ok &= check(category != nullptr && std::get<std::int32_t>(*category) == 1 && mode != nullptr &&
                        std::get<std::int32_t>(*mode) == 1,
                    "template defaults resolve both explicit and implicit enum values");
        auto overlaid = schema;
        const std::string defaults = "Category : default\n"
                                     "Emission : 1, 0.5*2, frac(Time/40)\n"
                                     "_TNormalMap : \"normal.png\"\n"
                                     "_VVolumeMap : smoke.dds\n";
        applyMaterialDefaultFile(overlaid, defaults, "materials");
        const auto& annotation = overlaid.defaultFileAnnotation;
        const auto value = std::find_if(annotation.values.begin(), annotation.values.end(),
                                        [](const auto& item) { return item.field == "Emission"; });
        const auto categoryExpression = std::find_if(annotation.values.begin(), annotation.values.end(),
                                                     [](const auto& item) { return item.field == "Category"; });
        const auto normal = std::find_if(annotation.textures.begin(), annotation.textures.end(),
                                         [](const auto& item) { return item.field == "NormalMap"; });
        const auto volume = std::find_if(annotation.textures.begin(), annotation.textures.end(),
                                         [](const auto& item) { return item.field == "VolumeMap"; });
        const auto* templateCategory = overlaid.defaults.find("Category");
        FxEvalContext expressionContext;
        expressionContext.time = 10.0;
        ok &= check(overlaid.defaultFileSourceText == defaults && value != annotation.values.end() &&
                        value->source == "1, 0.5*2, frac(Time/40)" && value->components.size() == 3 &&
                        std::abs(fxToDouble(evaluateFxExpr(value->components[1], expressionContext)) - 1.0) < 1.0e-6 &&
                        std::abs(fxToDouble(evaluateFxExpr(value->components[2], expressionContext)) - 0.25) < 1.0e-6 &&
                        categoryExpression != annotation.values.end() && categoryExpression->components.size() == 1 &&
                        fxToInt(evaluateFxExpr(categoryExpression->components[0], expressionContext)) == 0 &&
                        templateCategory != nullptr && std::get<std::int32_t>(*templateCategory) == 1 &&
                        normal != annotation.textures.end() && normal->path == "normal.png" && normal->index == 0 &&
                        normal->mipmapped && normal->baseDirectory == "materials" &&
                        volume != annotation.textures.end() && volume->dimension == MaterialTextureDimension::threeD &&
                        volume->path == "smoke.dds" && volume->index == 1 && volume->baseDirectory == "materials",
                    "default-file annotations retain expressions and indexed 2D/3D texture assignments");
        bool rejectedTemplateExpression = false;
        try {
            static_cast<void>(parseMaterialTemplateSchema("f.1 : Roughness\nRoughness : 0.5*2\n"));
        } catch (const std::invalid_argument&) {
            rejectedTemplateExpression = true;
        }
        ok &= check(rejectedTemplateExpression, "template defaults reject expressions per upstream MatDesc rules");
        bool rejectedUnknownTexture = false;
        try {
            applyMaterialDefaultFile(overlaid, "_TUnknown : unknown.png\n");
        } catch (const std::invalid_argument&) {
            rejectedUnknownTexture = true;
        }
        ok &= check(rejectedUnknownTexture, "default-file rejects undeclared texture assignments");
        bool rejectedWidth = false;
        try {
            static_cast<void>(parseMaterialTemplateSchema("f.5 : Invalid\n"));
        } catch (const std::invalid_argument&) {
            rejectedWidth = true;
        }
        ok &= check(rejectedWidth, "material schema rejects unsupported field widths");
        const auto rejects = [](std::string_view invalidSource) {
            try {
                static_cast<void>(parseMaterialTemplateSchema(invalidSource));
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };
        ok &= check(rejects("f.1 : Shared\ni.1 : Shared\n"),
                    "material schema rejects duplicate field names across scalar types");
        ok &= check(rejects("_T0 : Surface\n_V1 : Surface\n"),
                    "material schema rejects duplicate texture names across dimensions");
        ok &= check(rejects("_E Category : default=0, glass=1\n_E : Category : default=0, glass=1\n"),
                    "material schema rejects duplicate enum definitions for one field");
    }

    // Schema-based linking overlays instance values but emits an ordered
    // value list for the generated HLSL struct ABI.
    {
        const auto schema =
            parseMaterialTemplateSchema("f.1 : Roughness\nf.3 : Emission\ni.1 : Mode\n_T0m : AlbedoMap\n"
                                        "_E Mode : off=0, on=1\nRoughness : 0.5\nMode : on\n",
                                        "Surface");
        MaterialInstance instance;
        instance.templateName = "Surface";
        instance.overrides.set("Roughness", 0.25F);
        instance.overrides.set("Emission", std::array<float, 3>{1.0F, 2.0F, 3.0F});
        MaterialResourceDecl albedo;
        albedo.id = "AlbedoMap";
        albedo.texture.path = "textures/albedo.png";
        albedo.hasTexture = true;
        instance.extraResources.push_back(std::move(albedo));

        const auto plan = linkMaterial(schema, &instance);
        ok &= check(
            plan.orderedValues.size() == 3 && plan.orderedValues[0].schema.name == "Roughness" &&
                std::get<float>(plan.orderedValues[0].value) == 0.25F &&
                plan.orderedValues[1].schema.name == "Emission" &&
                std::get<std::array<float, 3>>(plan.orderedValues[1].value) == std::array<float, 3>{1.0F, 2.0F, 3.0F} &&
                plan.orderedValues[2].schema.name == "Mode" && std::get<std::int32_t>(plan.orderedValues[2].value) == 1,
            "material binding values preserve schema field order and resolved defaults/overrides");
        ok &= check(plan.layout.uniqueTextures.size() == 1 && plan.slotFor("AlbedoMap") != nullptr,
                    "schema texture declarations enter the existing deterministic resource linker");

        MaterialInstance invalid;
        invalid.overrides.set("Roughness", std::int32_t{1});
        bool rejectedType = false;
        try {
            static_cast<void>(linkMaterial(schema, &invalid));
        } catch (const std::invalid_argument&) {
            rejectedType = true;
        }
        ok &= check(rejectedType, "material linker rejects instance values with an incompatible schema type");
    }

    // Alias folding: shared / ref / shareTags collapse to canonical ids.
    {
        const auto templ = makeAliasTemplate();
        const auto layout = linkMaterialLayout(templ, nullptr);
        const auto& table = layout.localToCanonical;
        ok &= check(table.at("albedo") == "shared:albedo", "shared folds to shared:albedo");
        ok &= check(table.at("albedoCopy") == "shared:albedo", "ref folds to shared target");
        ok &= check(table.at("chain") == "shared:albedo", "chained ref folds transitively");
        ok &= check(table.at("baseA") == "tag:base" && table.at("baseB") == "tag:base", "shareTags fold to tag:base");
        ok &= check(table.at("local") == "local", "concrete id keeps its name");
        ok &= check(layout.slotForLocal("chain") == layout.slotForCanonical("shared:albedo"),
                    "aliased locals share one _R slot");
    }

    // Alias cycles canonicalize to the cycle itself, independent of the
    // entrypoint used to resolve the chain.
    {
        std::unordered_map<std::string, MaterialResourceDecl> cycle{
            {"A", {.id = "A", .ref = "B"}},
            {"B", {.id = "B", .ref = "C"}},
            {"C", {.id = "C", .ref = "B"}},
        };
        const auto a = resolveCanonicalResourceId("A", cycle);
        const auto b = resolveCanonicalResourceId("B", cycle);
        const auto c = resolveCanonicalResourceId("C", cycle);
        ok &= check(a == "B" && b == "B" && c == "B", "alias cycle has entrypoint-independent canonical id");
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
        const auto predicateOnly = compileFxCondition("if time > 0");
        ok &= check(predicateOnly.events == kFxEventNone && predicateOnly.predicate == "time > 0",
                    "condition predicate-only prefix");
        const auto parenthesized = compileFxCondition("when (FRAME > 0)");
        ok &= check(parenthesized.events == kFxEventNone && parenthesized.predicate == "(FRAME > 0)",
                    "condition parenthesized predicate-only prefix");
        const auto plus = compileFxCondition("start+resize");
        ok &= check(plus.events == (kFxEventStart | kFxEventResize), "condition plus-separated events");
        const auto models = compileFxCondition("on modelChanged, materialChanged");
        ok &=
            check(models.events == (kFxEventModelChanged | kFxEventMaterialChanged), "condition model/material events");
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
        ok &=
            check(toEffectPassType(FxPassOp{FxRasterOp{}}) == EffectPassType::rasterizer, "raster maps to rasterizer");
        ok &= check(toEffectPassType(FxPassOp{FxPostProcessOp{}}) == EffectPassType::postprocess,
                    "postprocess op maps to postprocess");
        ok &= check(toEffectPassType(FxPassOp{FxComputeOp{}}) == EffectPassType::compute, "compute maps to compute");
        ok &= check(toEffectPassType(FxPassOp{FxRayTracingOp{}}) == EffectPassType::raytracing,
                    "raytracing maps to raytracing");
        ok &= check(toEffectPassType(FxPassOp{FxCopyOp{}}) == EffectPassType::copy, "copy preserves legacy type");
        ok &= check(toEffectPassType(FxPassOp{FxClearRtvOp{}}) == EffectPassType::clear,
                    "clearRtv preserves legacy type");
        ok &= check(toEffectPassType(FxPassOp{FxClearUavOp{}}) == EffectPassType::clear,
                    "clearUav preserves legacy type");
        ok &= check(toEffectPassType(FxPassOp{FxMipmapGenOp{}}) == EffectPassType::mipmap,
                    "mipmapGen preserves legacy type");
        ok &= check(toEffectPassType(FxPassOp{FxOidnOp{}}) == EffectPassType::oidn, "oidn maps to host operation");
        ok &= check(std::holds_alternative<FxOidnOp>(fxPassOpFromEffectPassType(EffectPassType::oidn)),
                    "oidn preserves typed operation");
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
        bool unknownTypeThrew = false;
        try {
            static_cast<void>(fxPassOpFromEffectPassType(EffectPassType::unknown));
        } catch (const std::runtime_error&) {
            unknownTypeThrew = true;
        }
        ok &= check(unknownTypeThrew, "unknown legacy pass type is rejected");

        const FxPass copy{.name = "copy",
                          .category = FxCategory::postprocess,
                          .op = FxPassOp{FxCopyOp{.source = "source", .destination = "destination"}}};
        const auto copyLegacy = effectPassFromFxPass(copy);
        ok &= check(copyLegacy.type == EffectPassType::copy && copyLegacy.inputs.size() == 1 &&
                        copyLegacy.renderTargets.size() == 1,
                    "copy preserves legacy input/output contract");
        const auto copyRoundTrip = fxPassFromEffectPass(copyLegacy, FxCategory::postprocess);
        ok &= check(std::holds_alternative<FxCopyOp>(copyRoundTrip.op), "copy round-trips through EffectPass");

        const FxPass oidn{
            .name = "Denoise",
            .category = FxCategory::postprocess,
            .op = FxPassOp{FxOidnOp{.input = "Beauty", .output = "Denoised", .albedo = "Albedo", .normal = "Normal"}}};
        const auto oidnLegacy = effectPassFromFxPass(oidn);
        ok &= check(oidnLegacy.type == EffectPassType::oidn && oidnLegacy.oidnInput == "Beauty" &&
                        oidnLegacy.oidnAlbedo == "Albedo" && oidnLegacy.oidnNormal == "Normal" &&
                        oidnLegacy.oidnOutput == "Denoised",
                    "oidn preserves host input and output semantics");
        const auto oidnRoundTrip = fxPassFromEffectPass(oidnLegacy, FxCategory::postprocess);
        ok &= check(std::holds_alternative<FxOidnOp>(oidnRoundTrip.op) &&
                        std::get<FxOidnOp>(oidnRoundTrip.op).normal == "Normal",
                    "oidn round-trips through EffectPass");
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
        legacy.renderTargets = {{.name = "color", .clear = false, .clearValue = {}}};
        legacy.depth = {.name = "depth", .clear = true, .clearValue = {}};
        const auto converted = fxPassFromEffectPass(legacy, FxCategory::render);
        ok &= check(std::holds_alternative<FxRasterOp>(converted.op), "legacy raster becomes FxRasterOp");
        ok &= check(converted.category == FxCategory::render, "explicit category survives conversion");
        const auto roundTrip = effectPassFromFxPass(converted);
        ok &= check(roundTrip.type == EffectPassType::rasterizer && roundTrip.vertexShader == "vs",
                    "FxRasterOp round trips to EffectPass");
        const EffectPass computeLegacy{.name = "cs", .type = EffectPassType::compute, .computeShader = "cs_main"};
        const auto computeConverted = fxPassFromEffectPass(computeLegacy, "render");
        ok &= check(std::holds_alternative<FxComputeOp>(computeConverted.op), "legacy compute converts");

        EffectPass unknown;
        unknown.name = "malformed";
        bool unknownPassThrew = false;
        try {
            static_cast<void>(fxPassFromEffectPass(unknown, FxCategory::render));
        } catch (const std::runtime_error&) {
            unknownPassThrew = true;
        }
        ok &= check(unknownPassThrew, "unknown legacy pass is rejected");

        const FxPass utility{.name = "clear",
                             .category = FxCategory::postprocess,
                             .op = FxPassOp{FxClearRtvOp{.target = "color", .clear = true}}};
        const auto utilityLegacy = effectPassFromFxPass(utility);
        ok &= check(utilityLegacy.type == EffectPassType::clear && utilityLegacy.renderTargets.size() == 1,
                    "clear utility pass preserves legacy conversion");
    }

    if (!ok)
        std::cerr << "fx_linker_tests: FAILED\n";
    else
        std::cout << "fx_linker_tests: all checks passed\n";
    return ok ? 0 : 1;
}
