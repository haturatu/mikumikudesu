#include "core/project.hpp"
#include "graphics/preview_effects.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

namespace {
bool check(bool condition, const char* message) {
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}
} // namespace

int main() {
    using namespace dayo::graphics;
    bool ok = true;
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
        project.upstreamDocumentJson =
            R"({"mikumikudesu":{"renderSettings":{"futureEffect":17,"bloom":{"futureParameter":9}}}})";
        dayo::core::saveProject(path, project);
        const auto loaded = dayo::core::loadProject(path);
        const auto& value = loaded.renderSettings;
        ok &= check(value.viewTransform == PreviewViewTransform::agx && value.exposure == -1.5F &&
                        value.whiteBalance == 4200.0F && value.clearcoat.override == EffectOverride::enabled &&
                        value.clearcoat.intensity == 0.3F &&
                        value.ambientOcclusionMode == PreviewAmbientOcclusion::ssao &&
                        value.antiAliasing == PreviewAntiAliasing::off,
                    "project round trip preserves effect overrides, methods and color management");
        ok &= check(loaded.upstreamDocumentJson.find("futureEffect") != std::string::npos &&
                        loaded.upstreamDocumentJson.find("futureParameter") != std::string::npos,
                    "future effect settings survive saving");
        {
            std::ofstream file(path);
            file
                << R"({"mikumikudesu":{"version":3,"renderSettings":{"preset":-1,"antiAliasing":256,"viewTransform":"invalid","exposure":1000000,"whiteBalance":0,"bloom":{"override":256,"intensity":-5}}}})";
        }
        const auto malformed = dayo::core::loadProject(path).renderSettings;
        ok &= check(malformed.preset == PreviewQualityPreset::automatic &&
                        malformed.antiAliasing == PreviewAntiAliasing::automatic &&
                        malformed.viewTransform == PreviewViewTransform::aces && malformed.exposure == 12.0F &&
                        malformed.whiteBalance == 1000.0F && malformed.bloom.override == EffectOverride::inherit &&
                        malformed.bloom.intensity == 0.0F,
                    "malformed persisted settings use safe defaults without wrapping enum values");
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
