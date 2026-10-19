#pragma once

#include "graphics/preview_dof.hpp"
#include "graphics/preview_quality.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace dayo::graphics {

// Preview quality effects are described in three layers:
//
//   1. PreviewRenderSettings  - what the user asked for. Serialized into the
//      .dayo project and shown in the Render Settings panel. It never contains
//      GPU-dependent decisions, so explicit overrides survive a move between
//      different machines.
//   2. ResolvedPreviewEffects - what the device will actually run after the
//      quality preset, the per-effect override and the device capability limits
//      are applied. This is what the shaders consume.
//   3. PreviewEffectSupport   - which effects the current device/quality profile
//      is able to run at all, so the UI can explain a disabled checkbox instead
//      of silently ignoring it.
//
// Adding an effect means adding a PreviewEffectSetting, a preset default, a
// support bit and a field on the resolved struct. The shaders read the resolved
// values from the preview environment constant block.

// Three-state override: a project inherits the active preset by default, and can
// still force an individual effect on or off without losing that preset.
enum class EffectOverride : std::uint8_t { inherit, enabled, disabled };

enum class PreviewAmbientOcclusion : std::uint8_t { ssao, gtao };
enum class PreviewShadowFilter : std::uint8_t { pcf, pcss, automatic };
enum class PreviewViewTransform : std::uint8_t { aces, agx, neutral };
enum class PreviewAntiAliasing : std::uint8_t { off, fxaa, automatic };
enum class PreviewQualityPreset : std::uint8_t { automatic, performance, balanced, high };

struct PreviewEffectSetting {
    EffectOverride override{EffectOverride::inherit};
    float intensity{1.0F};
};

// User intent. Do not add fields that only a particular GPU can evaluate.
struct PreviewRenderSettings {
    PreviewQualityPreset preset{PreviewQualityPreset::automatic};
    PreviewAmbientOcclusion ambientOcclusionMode{PreviewAmbientOcclusion::gtao};
    PreviewShadowFilter shadowFilter{PreviewShadowFilter::automatic};
    PreviewViewTransform viewTransform{PreviewViewTransform::aces};
    PreviewAntiAliasing antiAliasing{PreviewAntiAliasing::automatic};
    float exposure{0.0F};        // EV applied before the view transform.
    float whiteBalance{6500.0F}; // Kelvin of the assumed white point.
    float bloomThreshold{1.0F};

    PreviewEffectSetting ambientOcclusion{};
    PreviewEffectSetting softShadows{};
    PreviewEffectSetting skinSubsurface{};
    PreviewEffectSetting hairShading{};
    PreviewEffectSetting clothSheen{};
    PreviewEffectSetting clearcoat{};
    PreviewEffectSetting bloom{};
    PreviewEffectSetting depthOfField{};
    PreviewDofSettings dof{};
};

// Older projects did not opt into the new postprocessing passes.
constexpr PreviewRenderSettings legacyPreviewRenderSettings() noexcept {
    PreviewRenderSettings settings;
    settings.ambientOcclusionMode = PreviewAmbientOcclusion::ssao;
    settings.shadowFilter = PreviewShadowFilter::pcf;
    settings.antiAliasing = PreviewAntiAliasing::off;
    settings.bloom.override = EffectOverride::disabled;
    settings.depthOfField.override = EffectOverride::disabled;
    settings.skinSubsurface.override = EffectOverride::disabled;
    settings.clothSheen.override = EffectOverride::disabled;
    settings.clearcoat.override = EffectOverride::disabled;
    return settings;
}

// Validate persisted and API-provided values before they reach GPU arithmetic.
inline PreviewRenderSettings sanitizePreviewRenderSettings(PreviewRenderSettings settings) noexcept {
    const auto finiteClamp = [](float value, float fallback, float low, float high) {
        return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
    };
    if (settings.preset > PreviewQualityPreset::high)
        settings.preset = PreviewQualityPreset::automatic;
    if (settings.ambientOcclusionMode > PreviewAmbientOcclusion::gtao)
        settings.ambientOcclusionMode = PreviewAmbientOcclusion::gtao;
    if (settings.shadowFilter > PreviewShadowFilter::automatic)
        settings.shadowFilter = PreviewShadowFilter::automatic;
    if (settings.viewTransform > PreviewViewTransform::neutral)
        settings.viewTransform = PreviewViewTransform::aces;
    if (settings.antiAliasing > PreviewAntiAliasing::automatic)
        settings.antiAliasing = PreviewAntiAliasing::automatic;
    settings.exposure = finiteClamp(settings.exposure, 0.0F, -12.0F, 12.0F);
    settings.whiteBalance = finiteClamp(settings.whiteBalance, 6500.0F, 1000.0F, 15000.0F);
    settings.bloomThreshold = finiteClamp(settings.bloomThreshold, 1.0F, 0.0F, 20.0F);
    for (auto* effect :
         {&settings.ambientOcclusion, &settings.softShadows, &settings.skinSubsurface, &settings.hairShading,
          &settings.clothSheen, &settings.clearcoat, &settings.bloom, &settings.depthOfField}) {
        if (effect->override > EffectOverride::disabled)
            effect->override = EffectOverride::inherit;
        effect->intensity = finiteClamp(effect->intensity, 1.0F, 0.0F, 4.0F);
    }
    settings.dof = sanitizePreviewDofSettings(std::move(settings.dof));
    return settings;
}

// Device/quality support. A clear bit means the Render Settings panel must show
// the reason instead of a working checkbox.
enum PreviewEffectSupport : std::uint32_t { // NOLINT(performance-enum-size)
    previewEffectSupportAmbientOcclusion = 1U << 0U,
    previewEffectSupportSoftShadows = 1U << 1U,
    previewEffectSupportSkinSubsurface = 1U << 2U,
    previewEffectSupportHairShading = 1U << 3U,
    previewEffectSupportClothSheen = 1U << 4U,
    previewEffectSupportClearcoat = 1U << 5U,
    previewEffectSupportBloom = 1U << 6U,
    previewEffectSupportDepthOfField = 1U << 7U,
};
constexpr std::uint32_t previewEffectSupportAll =
    previewEffectSupportAmbientOcclusion | previewEffectSupportSoftShadows | previewEffectSupportSkinSubsurface |
    previewEffectSupportHairShading | previewEffectSupportClothSheen | previewEffectSupportClearcoat |
    previewEffectSupportBloom | previewEffectSupportDepthOfField;

// Per-effect defaults for one quality preset.
struct PreviewEffectPresetDefaults {
    bool ambientOcclusion{};
    bool softShadows{};
    bool skinSubsurface{};
    bool hairShading{};
    bool clothSheen{};
    bool clearcoat{};
    bool bloom{};
    bool depthOfField{};
    PreviewShadowFilter shadowFilter{PreviewShadowFilter::pcf};
    PreviewAntiAliasing antiAliasing{PreviewAntiAliasing::off};
};

// The effective preset after the automatic slot is resolved against the device
// quality profile. A discrete GPU biases towards balanced, an APU towards
// performance, and an explicit `high` preview quality always selects high.
constexpr PreviewQualityPreset previewEffectivePreset(PreviewQualityPreset preset, PreviewQuality quality,
                                                      bool integrated) noexcept {
    if (preset != PreviewQualityPreset::automatic)
        return preset;
    if (quality == PreviewQuality::high)
        return PreviewQualityPreset::high;
    if (quality == PreviewQuality::apu || integrated)
        return PreviewQualityPreset::performance;
    return PreviewQualityPreset::balanced;
}

constexpr PreviewEffectPresetDefaults previewEffectPresetDefaults(PreviewQualityPreset preset) noexcept {
    switch (preset) {
    case PreviewQualityPreset::performance:
        // Keep character shading on an APU: it is the part of the image the
        // viewer looks at. Only the expensive extras are dropped.
        return {.ambientOcclusion = true,
                .softShadows = true,
                .skinSubsurface = true,
                .hairShading = true,
                .clothSheen = true,
                .clearcoat = false,
                .bloom = true,
                .depthOfField = false,
                .shadowFilter = PreviewShadowFilter::pcf,
                .antiAliasing = PreviewAntiAliasing::fxaa};
    case PreviewQualityPreset::high:
        return {.ambientOcclusion = true,
                .softShadows = true,
                .skinSubsurface = true,
                .hairShading = true,
                .clothSheen = true,
                .clearcoat = true,
                .bloom = true,
                .depthOfField = true,
                .shadowFilter = PreviewShadowFilter::pcss,
                .antiAliasing = PreviewAntiAliasing::fxaa};
    case PreviewQualityPreset::balanced:
    case PreviewQualityPreset::automatic:
    default:
        return {.ambientOcclusion = true,
                .softShadows = true,
                .skinSubsurface = true,
                .hairShading = true,
                .clothSheen = true,
                .clearcoat = true,
                .bloom = true,
                .depthOfField = false,
                .shadowFilter = PreviewShadowFilter::pcss,
                .antiAliasing = PreviewAntiAliasing::fxaa};
    }
}

// Effective per-frame state consumed by the renderer.
struct ResolvedPreviewEffects {
    float ambientOcclusionIntensity{1.0F};
    float shadowSoftness{1.0F};
    float skinSubsurfaceIntensity{1.0F};
    float hairIntensity{1.0F};
    float clothSheenIntensity{1.0F};
    float clearcoatIntensity{1.0F};
    float bloomIntensity{1.0F};
    float bloomThreshold{1.0F};
    float depthOfFieldIntensity{1.0F};
    float exposure{};
    float whiteBalance{6500.0F};
    bool ambientOcclusion{};
    PreviewAmbientOcclusion ambientOcclusionMode{PreviewAmbientOcclusion::gtao};
    bool softShadows{};
    PreviewShadowFilter shadowFilter{PreviewShadowFilter::pcf};
    bool skinSubsurface{};
    bool hairShading{};
    bool clothSheen{};
    bool clearcoat{};
    bool bloom{};
    bool depthOfField{};
    PreviewViewTransform viewTransform{PreviewViewTransform::aces};
    PreviewAntiAliasing antiAliasing{PreviewAntiAliasing::off};
};
static_assert(std::is_trivially_copyable_v<ResolvedPreviewEffects>);

constexpr bool effectEnabled(const PreviewEffectSetting& setting, bool presetDefault) noexcept {
    return setting.override == EffectOverride::inherit ? presetDefault : setting.override == EffectOverride::enabled;
}

constexpr float effectIntensity(const PreviewEffectSetting& setting, float fallback) noexcept {
    return setting.intensity >= 0.0F ? setting.intensity : fallback;
}

// `stillQuality` requests the export preset, which is only honoured while the
// user keeps the preset on automatic.
constexpr ResolvedPreviewEffects resolvePreviewEffects(const PreviewRenderSettings& settings, PreviewQuality quality,
                                                       bool integrated, bool stillQuality = false,
                                                       std::uint32_t support = previewEffectSupportAll) noexcept {
    const auto effectivePreset = previewEffectivePreset(settings.preset, quality, integrated);
    const auto preset = previewEffectPresetDefaults(stillQuality && settings.preset == PreviewQualityPreset::automatic
                                                        ? PreviewQualityPreset::high
                                                        : effectivePreset);
    ResolvedPreviewEffects resolved;
    resolved.ambientOcclusion = effectEnabled(settings.ambientOcclusion, preset.ambientOcclusion) &&
                                (support & previewEffectSupportAmbientOcclusion) != 0;
    resolved.ambientOcclusionMode = settings.ambientOcclusionMode;
    resolved.ambientOcclusionIntensity = effectIntensity(settings.ambientOcclusion, 1.0F);
    resolved.softShadows =
        effectEnabled(settings.softShadows, preset.softShadows) && (support & previewEffectSupportSoftShadows) != 0;
    resolved.shadowFilter = !resolved.softShadows                                     ? PreviewShadowFilter::pcf
                            : settings.shadowFilter == PreviewShadowFilter::automatic ? preset.shadowFilter
                                                                                      : settings.shadowFilter;
    resolved.shadowSoftness = effectIntensity(settings.softShadows, 1.0F);
    resolved.skinSubsurface = effectEnabled(settings.skinSubsurface, preset.skinSubsurface) &&
                              (support & previewEffectSupportSkinSubsurface) != 0;
    resolved.skinSubsurfaceIntensity = effectIntensity(settings.skinSubsurface, 1.0F);
    resolved.hairShading =
        effectEnabled(settings.hairShading, preset.hairShading) && (support & previewEffectSupportHairShading) != 0;
    resolved.hairIntensity = effectIntensity(settings.hairShading, 1.0F);
    resolved.clothSheen =
        effectEnabled(settings.clothSheen, preset.clothSheen) && (support & previewEffectSupportClothSheen) != 0;
    resolved.clothSheenIntensity = effectIntensity(settings.clothSheen, 1.0F);
    resolved.clearcoat =
        effectEnabled(settings.clearcoat, preset.clearcoat) && (support & previewEffectSupportClearcoat) != 0;
    resolved.clearcoatIntensity = effectIntensity(settings.clearcoat, 1.0F);
    resolved.bloom = effectEnabled(settings.bloom, preset.bloom) && (support & previewEffectSupportBloom) != 0;
    resolved.bloomIntensity = effectIntensity(settings.bloom, 1.0F);
    resolved.bloomThreshold = settings.bloomThreshold;
    resolved.depthOfField =
        effectEnabled(settings.depthOfField, preset.depthOfField) && (support & previewEffectSupportDepthOfField) != 0;
    resolved.depthOfFieldIntensity = effectIntensity(settings.depthOfField, 1.0F);
    // Colour management and anti-aliasing are single-choice settings, so the
    // preset only supplies the default when the user left them untouched.
    resolved.viewTransform = settings.viewTransform;
    resolved.exposure = settings.exposure;
    resolved.whiteBalance = settings.whiteBalance;
    resolved.antiAliasing =
        settings.antiAliasing == PreviewAntiAliasing::automatic ? preset.antiAliasing : settings.antiAliasing;
    return resolved;
}

// Layout of the effect parameter block appended to the preview environment
// constant buffer. Keep the indices in sync with preview.hlsl.
//   data[10] = ambient occlusion / skin / hair / cloth intensities
//   data[11] = clearcoat / shadow softness / bloom / depth of field intensities
//   data[12] = exposure EV / white balance / bloom threshold
//   data[13] = ambient occlusion / skin / hair / cloth enable flags
//   data[14] = clearcoat / PCSS / bloom / depth of field enable flags
//   data[15].x = 1 when GTAO replaced the legacy screen-space occlusion
constexpr std::uint32_t previewEffectDataIndex = 10;
constexpr std::uint32_t previewEffectDataFloats = 6 * 4;

constexpr std::array<float, previewEffectDataFloats>
packPreviewEffectData(const ResolvedPreviewEffects& effects) noexcept {
    return {effects.ambientOcclusionIntensity,
            effects.skinSubsurfaceIntensity,
            effects.hairIntensity,
            effects.clothSheenIntensity,
            effects.clearcoatIntensity,
            effects.shadowSoftness,
            effects.bloomIntensity,
            effects.depthOfFieldIntensity,
            effects.exposure,
            effects.whiteBalance,
            effects.bloomThreshold,
            0.0F,
            effects.ambientOcclusion ? 1.0F : 0.0F,
            effects.skinSubsurface ? 1.0F : 0.0F,
            effects.hairShading ? 1.0F : 0.0F,
            effects.clothSheen ? 1.0F : 0.0F,
            effects.clearcoat ? 1.0F : 0.0F,
            effects.shadowFilter == PreviewShadowFilter::pcss ? 1.0F : 0.0F,
            effects.bloom ? 1.0F : 0.0F,
            effects.depthOfField ? 1.0F : 0.0F,
            effects.ambientOcclusionMode == PreviewAmbientOcclusion::gtao ? 1.0F : 0.0F,
            static_cast<float>(effects.viewTransform),
            effects.antiAliasing == PreviewAntiAliasing::fxaa ? 1.0F : 0.0F,
            effects.softShadows ? 1.0F : 0.0F};
}

} // namespace dayo::graphics
