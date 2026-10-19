#include "app/preview_focus.hpp"
#include "core/project.hpp"
#include "graphics/preview_aperture.hpp"
#include "graphics/preview_effects.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>

namespace {
bool check(bool condition, const char* message) {
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}
bool focusAndLensMath() {
    using namespace dayo::graphics;
    bool ok = true;
    PreviewDofSettings lensSettings;
    lensSettings.fNumber = 1.4F;
    const auto open = previewDofLens(lensSettings, 3.0F, 0.785398163F, 1080, PreviewDofQuality::high);
    lensSettings.fNumber = 5.6F;
    const auto closed = previewDofLens(lensSettings, 3.0F, 0.785398163F, 1080, PreviewDofQuality::high);
    ok &= check(std::abs(open.cocScale / closed.cocScale - 4.0F) < 0.0001F,
                "f/1.4 produces four times the CoC radius of f/5.6");
    const auto coc = [&](float z) { return open.cocScale * (open.focusMetres / z - 1.0F); };
    ok &= check(coc(1.0F) > 0.0F && coc(5.0F) < 0.0F && coc(open.focusMetres) == 0.0F,
                "signed CoC distinguishes near, focus and far");
    const auto half = previewDofLens(lensSettings, 3.0F, 0.785398163F, 540, PreviewDofQuality::performance);
    ok &= check(std::abs(half.cocScale * 2.0F - closed.cocScale) < 0.0001F && half.samples < closed.samples,
                "CoC scales with resolution and APU quality reduces sampling");
    PreviewFocusCamera camera{{0.3F, -0.7F, 0.2F}, {0.3F, -0.5F, 0.4F}, 3.0F, 0.6F, true};
    const auto viewPoint = previewFocusViewPoint(camera, camera.target);
    ok &= check(std::abs(viewPoint[2] - camera.distance) < 0.0001F, "camera target focus uses axial depth");
    for (const bool perspective : {true, false}) {
        camera.perspective = perspective;
        constexpr float z = 2.0F;
        const float depth = perspective ? 100.0F / 99.95F - 5.0F / (99.95F * z) : (z - 0.05F) / 99.95F;
        const auto world = previewFocusUnproject(camera, {0.2F, 0.7F}, 1.5F, depth);
        ok &= check(world.has_value(), "perspective and orthographic picks unproject");
        if (world) {
            const auto view = previewFocusViewPoint(camera, *world);
            const float extent = std::tan(camera.verticalFovRadians * 0.5F) * (perspective ? z : camera.distance);
            ok &= check(std::abs(view[0] + 0.6F * 1.5F * extent) < 0.0001F &&
                            std::abs(view[1] + 0.4F * extent) < 0.0001F && std::abs(view[2] - z) < 0.0001F,
                        "pick round trip respects pitch, yaw, roll, target and projection");
        }
    }
    ok &= check(!previewFocusUnproject(camera, {0.5F, 0.5F}, 1.0F, 1.0F) &&
                    !previewFocusUnproject(camera, {-0.1F, 0.5F}, 1.0F, 0.5F) &&
                    !previewFocusUnproject(camera, {0.5F, 0.5F}, 1.0F, std::numeric_limits<float>::quiet_NaN()),
                "sky, invalid coordinates and nonfinite depth cannot become focus targets");
    std::vector<dayo::core::ModelInstance> models(1);
    auto& model = models.front();
    model.id = 17;
    model.model = std::make_shared<dayo::core::PmxModel>();
    model.normalization = {{1.0F, 2.0F, 3.0F}, 0.5F};
    model.model->bones.resize(3);
    model.model->bones[0].name = "左目";
    model.model->bones[0].position = {0.8F, 4.0F, 3.0F};
    model.model->bones[1].name = "右目";
    model.model->bones[1].position = {1.2F, 4.0F, 3.0F};
    model.model->bones[2].name = "頭";
    model.model->bones[2].position = {1.0F, 3.0F, 3.0F};
    dayo::core::fx::SceneEvaluationSnapshot snapshot;
    snapshot.models.resize(1);
    snapshot.models[0].id = 17;
    snapshot.models[0].bones.resize(3);
    for (auto& bone : snapshot.models[0].bones)
        bone.translation = {0.0F, 0.0F, 2.0F};
    lensSettings.focusMode = PreviewFocusMode::eyes;
    const auto eyes = dayo::app::previewFocusBonePoint(model, snapshot, lensSettings);
    ok &= check(eyes && std::abs((*eyes)[0]) < 0.0001F && (*eyes)[1] == 1.0F && (*eyes)[2] == 1.0F,
                "eye midpoint uses evaluated skinning transforms and model normalization");
    lensSettings.focusMode = PreviewFocusMode::face;
    const auto face = dayo::app::previewFocusBonePoint(model, snapshot, lensSettings);
    ok &= check(face && eyes && std::abs((*face)[1] - (*eyes)[1]) > 0.1F && (*face)[2] < (*eyes)[2],
                "face uses the head front anchor independently of the eye midpoint");
    model.model->bones.resize(1);
    model.model->bones[0].name = "Head";
    lensSettings.focusMode = PreviewFocusMode::face;
    ok &= check(dayo::app::previewFocusBonePoint(model, snapshot, lensSettings).has_value(),
                "face falls back to head aliases");
    lensSettings.focusMode = PreviewFocusMode::bone;
    lensSettings.boneName = "missing";
    camera = {};
    ok &= check(dayo::app::resolvePreviewFocusDistance(lensSettings, camera, models, snapshot) == camera.distance,
                "missing named bones safely fall back to the camera target");
    lensSettings.boneName = "Head";
    // A 180 degree bind-point rotation must affect focus, not only translation.
    snapshot.models[0].bones[0].rotation = {0.0F, 1.0F, 0.0F, 0.0F};
    const auto rotated = dayo::app::previewFocusBonePoint(model, snapshot, lensSettings);
    ok &= check(rotated && std::abs((*rotated)[2] + 2.0F) < 0.0001F,
                "animated bone focus rotates the bind position before translating");
    models.resize(2);
    models[0].sourcePath = "character-a.pmx";
    models[1].sourcePath = "character-b.pmx";
    std::array<dayo::core::ProjectModelState, 3> references{};
    references[0].source = "character-b.pmx";
    references[1].source = "missing.pmx";
    references[2].source = "character-a.pmx";
    ok &= check(dayo::app::previewFocusProjectIndex(models, references, 0) == 2 &&
                    dayo::app::previewFocusSceneIndex(models, references, 2) == 0 &&
                    dayo::app::previewFocusSceneIndex(models, references, 0) == 1,
                "focus model references survive different project metadata and loading order");
    ok &= check(dayo::app::previewFocusSceneIndex(models, references, 1) == dayo::app::previewMissingFocusModel,
                "a failed model load must not retarget focus to the next loaded model");
    return ok;
}
bool focusTrackingAndResponse() {
    using namespace dayo::graphics;
    using namespace dayo::core;
    using namespace dayo::app;
    bool ok = true;
    PreviewDofSettings settings;
    settings.focusDeadbandMetres = 0.0F;
    settings.focusResponseSeconds = 0.12F;
    PreviewFocusState slow{2.0F, 2.0F, 0.0F, true};
    auto fast = slow;
    for (int i = 0; i < 30; ++i)
        updatePreviewFocusState(slow, 2.2F, 1.0F / 30.0F, settings, true);
    for (int i = 0; i < 120; ++i)
        updatePreviewFocusState(fast, 2.2F, 1.0F / 120.0F, settings, true);
    ok &= check(std::abs(slow.currentMetres - fast.currentMetres) < 0.00001F && slow.currentMetres <= 2.2F,
                "AF exponential response is refresh-rate independent and cannot overshoot");
    settings.focusDeadbandMetres = 0.005F;
    const auto stable = fast.currentMetres;
    updatePreviewFocusState(fast, stable + 0.004F, 1.0F / 60.0F, settings, true);
    ok &= check(fast.currentMetres == stable && fast.velocity == 0.0F, "AF deadband avoids focus hunting");
    settings.focusMaxSpeed = 0.5F;
    updatePreviewFocusState(fast, 10.0F, 0.1F, settings, true);
    ok &= check(fast.currentMetres <= stable + 0.05001F && fast.velocity <= 0.50001F,
                "AF respects its maximum travel speed");
    updatePreviewFocusState(fast, 4.0F, -1.0F, settings, true);
    ok &= check(fast.currentMetres == 4.0F && fast.velocity == 0.0F, "backward seeks reset the AF history");
    updatePreviewFocusState(fast, 3.0F, 0.5F, settings, true);
    ok &= check(fast.currentMetres == 3.0F, "discontinuous timeline jumps reset the AF history");
    updatePreviewFocusState(fast, 2.0F, 0.01F, settings, false);
    ok &= check(!fast.trackingValid && fast.currentMetres == 2.0F,
                "missing targets use the resolved fallback immediately");

    ModelInstance instance;
    instance.id = 23;
    instance.cloneCount = 2;
    instance.model = std::make_shared<PmxModel>();
    instance.normalization = {{0.0F, 0.0F, 0.0F}, 1.0F};
    instance.model->bones.resize(1);
    instance.model->bones[0].name = "head";
    instance.model->bones[0].position = {0.0F, 1.0F, 0.0F};
    VmdMotion motion;
    VmdBoneKey first;
    first.name = "head";
    first.interpolation.fill(127);
    auto last = first;
    last.frame = 1;
    last.translation = {0.0F, 0.0F, 1.0F};
    motion.bones = {first, last};
    motion.lastFrame = 1;
    MmdAnimator animator(*instance.model);
    animator.setMotion(&motion);
    fx::SceneEvaluationSnapshot snapshot;
    snapshot.models.resize(1);
    snapshot.models.front().id = instance.id;
    settings.focusMode = PreviewFocusMode::bone;
    settings.boneName = "head";
    float previousZ = -1.0F;
    for (const float frame : {0.0F, 0.25F, 0.5F, 0.75F, 1.0F}) {
        const auto pose = animator.evaluate(frame, frame == 0.0F ? 0.0F : 1.0F / 120.0F, true);
        snapshot.models.front().bones = pose.bones;
        const auto target = previewFocusBonePoint(instance, snapshot, settings);
        ok &= check(target && (*target)[2] > previousZ,
                    "focus shares each fractional VMD pose instead of stepping at integer frames");
        if (target)
            previousZ = (*target)[2];
    }
    snapshot.models.front().bones.front().rotation = {0.0F, 1.0F, 0.0F, 0.0F};
    const std::array bindings{
        PreviewFocusModelBinding{instance.model, instance.normalization, instance.id, 4, 2, 0, true}};
    PreviewFocusSample sample;
    sample.materialIndex = 5;
    sample.instanceIndex = 1;
    sample.instanceCount = 2;
    sample.bones.push_back(makePreviewBone(snapshot.models.front().bones.front(), instance.normalization));
    sample.worldPoint = {1.3F, 1.1F, 0.7F};
    const auto tracked = previewTrackedFocus(sample, bindings, snapshot, settings.metresPerUnit);
    ok &= check(tracked && tracked->model == instance.id && tracked->instance == 1 && tracked->boneName == "head",
                "tracking binds to the GPU-selected model and clone");
    if (tracked) {
        settings.focusMode = PreviewFocusMode::trackedSurface;
        settings.localOffsetMetres = tracked->localOffsetMetres;
        settings.trackingInstance = tracked->instance;
        const auto point = previewFocusBonePoint(instance, snapshot, settings);
        ok &= check(point && std::abs((*point)[0] - sample.worldPoint[0]) < 0.00001F &&
                        std::abs((*point)[1] - sample.worldPoint[1]) < 0.00001F &&
                        std::abs((*point)[2] - sample.worldPoint[2]) < 0.00001F,
                    "a rotated bone-local clicked anchor round trips through clone placement");
        snapshot.models.front().bones.front().translation[2] += 0.4F;
        const auto moved = previewFocusBonePoint(instance, snapshot, settings);
        ok &= check(moved && std::abs((*moved)[2] - sample.worldPoint[2] - 0.4F) < 0.00001F,
                    "tracking moves with the bone while the static world point stays fixed");
    }
    sample.materialIndex = 3;
    ok &= check(!previewTrackedFocus(sample, bindings, snapshot, settings.metresPerUnit),
                "unidentified surfaces cannot silently bind to a nearby model");
    return ok;
}

bool apertureShape() {
    using namespace dayo::graphics;
    bool ok = true;
    const auto circle = previewApertureLut(0, 0.0F, 0.0F);
    const auto polygon = previewApertureLut(5, 0.0F, 0.0F);
    const auto rounded = previewApertureLut(5, 1.0F, 0.0F);
    const auto rotated = previewApertureLut(5, 0.0F, 36.0F);
    const auto area = [](const auto& lut) { return std::accumulate(lut.begin(), lut.end(), 0.0F); };
    ok &= check(std::abs(area(circle) / static_cast<float>(circle.size()) - 0.785398F) < 0.001F &&
                    std::abs(area(polygon) / static_cast<float>(polygon.size()) - 0.59441F) < 0.002F,
                "aperture coverage integrates circle and regular pentagon areas");
    ok &= check(area(rounded) > area(polygon) && std::abs(area(rounded) - area(circle)) / area(circle) < 0.002F,
                "cubic blade curvature smoothly approaches the circular aperture");
    ok &= check(polygon != rotated && std::abs(area(polygon) - area(rotated)) < 16.0F,
                "rotating aperture blades changes the mask while preserving area");
    ok &= check(std::ranges::all_of(polygon, [](float value) { return value >= 0.0F && value <= 1.0F; }) &&
                    std::ranges::any_of(polygon, [](float value) { return value > 0.0F && value < 1.0F; }),
                "aperture boundaries contain antialiased fractional coverage");
    PreviewDofSettings settings;
    settings.maxRadiusPixels = 96.0F;
    const auto lens = previewDofLens(settings, 3.0F, 0.7F, 540, PreviewDofQuality::performance);
    ok &= check(lens.maxRadius == 48.0F && lens.samples == 12,
                "an explicit radius cap scales with resolution without increasing the APU sample budget");
    settings.apertureBlades = 3;
    settings.apertureCurvature = 4.0F;
    settings.maxRadiusPixels = 999.0F;
    settings = sanitizePreviewDofSettings(settings);
    ok &=
        check(settings.apertureBlades == 0 && settings.apertureCurvature == 1.0F && settings.maxRadiusPixels == 128.0F,
              "invalid aperture shapes and unbounded radii are sanitized");
    return ok;
}

} // namespace

int main() {
    using namespace dayo::graphics;
    bool ok = focusAndLensMath();
    ok &= focusTrackingAndResponse();
    ok &= apertureShape();
    PreviewRenderSettings settings;
    auto effective = resolvePreviewEffects(settings, PreviewQuality::apu, true);
    ok &= check(effective.shadowFilter == PreviewShadowFilter::pcf && !effective.depthOfField,
                "automatic APU profile avoids PCSS and DoF");
    effective = resolvePreviewEffects(settings, PreviewQuality::high, true);
    ok &= check(effective.shadowFilter == PreviewShadowFilter::pcss && effective.depthOfField,
                "high profile selects PCSS and DoF");
    settings.preset = PreviewQualityPreset::performance;
    effective = resolvePreviewEffects(settings, PreviewQuality::high, false, true);
    ok &= check(!effective.depthOfField && effective.shadowFilter == PreviewShadowFilter::pcf,
                "export preserves an explicit performance preset");
    settings.preset = PreviewQualityPreset::automatic;
    effective = resolvePreviewEffects(settings, PreviewQuality::apu, true, true);
    ok &= check(effective.depthOfField, "automatic still export selects high");
    const auto unsupported = resolvePreviewEffects(settings, PreviewQuality::high, false, false, 0);
    ok &= check(!unsupported.ambientOcclusion && !unsupported.bloom && !unsupported.depthOfField &&
                    !unsupported.skinSubsurface,
                "unavailable effects are not reported active");
    settings.bloom.override = EffectOverride::disabled;
    settings.depthOfField.override = EffectOverride::enabled;
    settings.antiAliasing = PreviewAntiAliasing::off;
    settings.shadowFilter = PreviewShadowFilter::pcf;
    settings.ambientOcclusion.intensity = 0.0F;
    effective = resolvePreviewEffects(settings, PreviewQuality::high, false);
    ok &= check(!effective.bloom && effective.depthOfField && effective.antiAliasing == PreviewAntiAliasing::off &&
                    effective.shadowFilter == PreviewShadowFilter::pcf && effective.ambientOcclusionIntensity == 0.0F,
                "explicit OFF, zero intensity and method choices override presets");
    settings.bloom.override = EffectOverride::inherit;
    ok &=
        check(resolvePreviewEffects(settings, PreviewQuality::high, false).bloom, "resetting override restores preset");
    settings.exposure = std::numeric_limits<float>::infinity();
    settings.whiteBalance = std::numeric_limits<float>::quiet_NaN();
    settings.hairShading.intensity = -1.0F;
    settings.bloomThreshold = 999.0F;
    const auto sanitized = sanitizePreviewRenderSettings(settings);
    ok &= check(sanitized.exposure == 0.0F && sanitized.whiteBalance == 6500.0F &&
                    sanitized.hairShading.intensity == 0.0F && sanitized.bloomThreshold == 20.0F,
                "nonfinite and out of range values cannot reach GPU math");

    const auto path = std::filesystem::temp_directory_path() / "mikumikudesu-render-settings-test.dayo";
    try {
        dayo::core::DayoProject project;
        project.renderSettings = sanitized;
        project.renderSettings.viewTransform = PreviewViewTransform::agx;
        project.renderSettings.exposure = -1.5F;
        project.renderSettings.whiteBalance = 4200.0F;
        project.renderSettings.clearcoat = {EffectOverride::enabled, 0.3F};
        project.renderSettings.ambientOcclusionMode = PreviewAmbientOcclusion::ssao;
        auto& dof = project.renderSettings.dof;
        dof.focusMode = PreviewFocusMode::trackedSurface;
        dof.quality = PreviewDofQuality::high;
        dof.fNumber = 1.4F;
        dof.temporal = false;
        dof.apertureBlades = 7;
        dof.apertureCurvature = 0.35F;
        dof.apertureRotationDegrees = 28.0F;
        dof.maxRadiusPixels = 64.0F;
        dof.focusDistanceMetres = 1.8F;
        dof.focusOffsetMetres = -0.05F;
        dof.metresPerUnit = 0.6F;
        dof.sensorHeightMm = 18.0F;
        dof.modelIndex = 2;
        dof.boneName = "右目";
        dof.worldPoint = {1.0F, 2.0F, 3.0F};
        dof.localOffsetMetres = {0.1F, -0.2F, 0.3F};
        dof.faceOffsetMetres = {0.0F, 0.12F, -0.08F};
        dof.focusResponseSeconds = 0.2F;
        dof.focusDeadbandMetres = 0.007F;
        dof.focusMaxSpeed = 2.0F;
        dof.trackingInstance = 3;
        project.upstreamDocumentJson =
            R"({"mikumikudesu":{"renderSettings":{"futureEffect":17,"bloom":{"futureParameter":9},"dof":{"futureLensParameter":7}}}})";
        dayo::core::saveProject(path, project);
        const auto loaded = dayo::core::loadProject(path);
        const auto& value = loaded.renderSettings;
        ok &= check(value.viewTransform == PreviewViewTransform::agx && value.exposure == -1.5F &&
                        value.whiteBalance == 4200.0F && value.clearcoat.override == EffectOverride::enabled &&
                        value.clearcoat.intensity == 0.3F &&
                        value.ambientOcclusionMode == PreviewAmbientOcclusion::ssao &&
                        value.antiAliasing == PreviewAntiAliasing::off,
                    "project round trip preserves effect overrides, methods and color management");
        ok &= check(
            value.dof.focusMode == dof.focusMode && value.dof.quality == dof.quality &&
                value.dof.fNumber == dof.fNumber && value.dof.temporal == dof.temporal &&
                value.dof.apertureBlades == dof.apertureBlades &&
                value.dof.apertureCurvature == dof.apertureCurvature &&
                value.dof.apertureRotationDegrees == dof.apertureRotationDegrees &&
                value.dof.maxRadiusPixels == dof.maxRadiusPixels &&
                value.dof.focusDistanceMetres == dof.focusDistanceMetres &&
                value.dof.focusOffsetMetres == dof.focusOffsetMetres && value.dof.metresPerUnit == dof.metresPerUnit &&
                value.dof.sensorHeightMm == dof.sensorHeightMm && value.dof.modelIndex == dof.modelIndex &&
                value.dof.boneName == dof.boneName && value.dof.worldPoint == dof.worldPoint &&
                value.dof.localOffsetMetres == dof.localOffsetMetres &&
                value.dof.faceOffsetMetres == dof.faceOffsetMetres &&
                value.dof.focusResponseSeconds == dof.focusResponseSeconds &&
                value.dof.focusDeadbandMetres == dof.focusDeadbandMetres &&
                value.dof.focusMaxSpeed == dof.focusMaxSpeed && value.dof.trackingInstance == dof.trackingInstance,
            "all lens and stable focus target fields survive project reload");
        ok &= check(loaded.upstreamDocumentJson.find("futureLensParameter") != std::string::npos,
                    "unknown lens fields survive project saving");
        ok &= check(loaded.upstreamDocumentJson.find("futureEffect") != std::string::npos &&
                        loaded.upstreamDocumentJson.find("futureParameter") != std::string::npos,
                    "future effect settings survive saving");
        {
            std::ofstream file(path);
            file
                << R"({"mikumikudesu":{"version":3,"renderSettings":{"preset":-1,"antiAliasing":256,"viewTransform":"invalid","exposure":1000000,"whiteBalance":0,"bloom":{"override":256,"intensity":-5},"dof":{"focusMode":999,"quality":-1,"fNumber":0,"sensorHeightMm":-3,"metresPerUnit":0,"focusDistanceMetres":-1,"worldPoint":["bad",{},2],"modelIndex":-1,"boneName":3}}}})";
        }
        const auto malformed = dayo::core::loadProject(path).renderSettings;
        ok &= check(malformed.preset == PreviewQualityPreset::automatic &&
                        malformed.antiAliasing == PreviewAntiAliasing::automatic &&
                        malformed.viewTransform == PreviewViewTransform::aces && malformed.exposure == 12.0F &&
                        malformed.whiteBalance == 1000.0F && malformed.bloom.override == EffectOverride::inherit &&
                        malformed.bloom.intensity == 0.0F,
                    "malformed persisted settings use safe defaults without wrapping enum values");
        ok &= check(malformed.dof.focusMode == PreviewFocusMode::cameraTarget &&
                        malformed.dof.quality == PreviewDofQuality::automatic && malformed.dof.fNumber == 0.7F &&
                        malformed.dof.metresPerUnit == 0.01F && malformed.dof.focusDistanceMetres == 0.05F &&
                        malformed.dof.modelIndex == 0 && malformed.dof.boneName.empty(),
                    "malformed lens fields are validated without division by zero or enum wrap");
        {
            std::ofstream file(path);
            file << R"({"mikumikudesu":{"version":3}})";
        }
        const auto legacy = dayo::core::loadProject(path).renderSettings;
        ok &= check(legacy.preset == PreviewQualityPreset::automatic && legacy.exposure == 0.0F &&
                        legacy.whiteBalance == 6500.0F && legacy.viewTransform == PreviewViewTransform::aces &&
                        legacy.bloom.override == EffectOverride::disabled &&
                        legacy.antiAliasing == PreviewAntiAliasing::off,
                    "old projects load with default settings");
        std::filesystem::remove(path);
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: settings persistence: " << exception.what() << '\n';
        return 1;
    }
    return ok ? 0 : 1;
}
