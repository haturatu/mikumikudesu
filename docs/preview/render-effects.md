# Preview render effects

Open **Effects** in the viewport toolbar, or switch to the **Render** workspace.
The Inspector groups settings into Lighting, Materials and Post processing.
The controls apply to the Vulkan Preview renderer and Preview exports.

![Japanese Render Settings panel](render-settings-ja.png)

[English panel screenshot](render-settings-en.png)

## Presets and overrides

Auto uses Performance on an integrated GPU/APU and Balanced on a discrete GPU;
`--preview-quality high` selects High. Performance keeps character shading,
GTAO, PCF shadows, Bloom and FXAA, but skips Clearcoat and depth of field.
Balanced adds Clearcoat and PCSS. High also enables depth of field.

A checkbox shows the resolved effect state. Changing it creates an explicit
On/Off override without changing the selected preset. The adjacent Reset button
restores that effect's preset state and intensity. Reset effect overrides resets
all effect overrides, shadow filter and anti-aliasing to their preset defaults.
Color management and AO method are separate choices. Off for anti-aliasing
really disables it; Auto inherits the preset. Zero intensity is supported.

Still-quality exports select High only when the preset is Auto. Explicit presets
and effect overrides survive export. Settings are stored in the optional
`mikumikudesu.renderSettings` object of `.dayo` version 3 files. Older projects
retain ACES, legacy SSAO, PCF and no new Bloom/FXAA/DoF; the new settings can be
selected after loading. Unknown effect fields survive save/load. Invalid enums
fall back safely, and numeric values are checked for finiteness and clamped.

## Effects and limitations

| Control | Implementation | Scope |
| --- | --- | --- |
| AO | Four slice GTAO horizon search in reconstructed view space, analytic cosine integral, depth/normal bilateral denoising; legacy SSAO selectable | Screen-space geometry only; fixed 0.15 scene-unit radius, reduced resolution follows device quality |
| Skin scattering | Wrapped/transmitted skin lighting plus screen-space RGB diffusion of a separate skin **diffuse-only** HDR attachment | Skin preset; depth and skin masks reject non-skin samples; opaque skin only; requires independent attachment blending |
| Hair highlights | Two shifted Kajiya–Kay lobes with UV-derivative tangent fallback | Hair preset; no direction texture |
| Cloth sheen | Charlie distribution tinted by base color | Cloth preset |
| Clearcoat | Additional dielectric GGX lobe with base-lobe attenuation | Plastic and Glass presets; does not add glass refraction |
| Soft shadows | PCF or PCSS blocker search and bounded variable filter radius | Existing directional shadow map/projection; no contact ray tracing |
| Bloom | Half-resolution threshold prefilter and separable Gaussian blur before tone mapping | Three compute dispatches; skipped when disabled; no spectral glare |
| View transform | ACES fit, compact AgX approximation, or Khronos PBR Neutral | SDR linear sRGB output followed by hardware sRGB transfer; not Blender OCIO/LUT parity |
| Exposure / white balance | EV gain and normalized Planckian RGB white-point correction | Approximate white balance, neutral at 6500 K |
| Anti-aliasing | Off / FXAA / Auto | Spatial filter; no temporal history |
| Depth of field | Depth-weighted ring blur, circle of confusion capped at 18 pixels at intensity 1 | Focus follows camera target distance; perspective and orthographic depth supported |

Skin diffuse is separate from specular highlights and hair, so diffusion does
not smear the final scene image. Transparent overlays and outlines erase the
skin mask where they draw; this conservatively leaves these boundaries sharp.
The low-cost transmission approximation also remains useful for backlit skin.
Screen-space effects cannot recover hidden or off-screen geometry. SSR, SSGI,
TAA, refractive glass and physically based camera bokeh remain separate work.

The previous-frame background mode uses the direct shading path. Color
management and material lighting still apply, but HDR postprocessing is bypassed
in that mode. Normal previews and exported frames use the HDR path.

## Runtime behavior

Switches reuse compiled shaders and pipelines. Effect constants use a new
storage buffer whose old allocation is retired behind frame fences, so GUI
changes do not call `vkDeviceWaitIdle()` or overwrite in-flight uniforms. Bloom
images and tone mapping descriptors belong to each frame slot. Disabled Bloom
skips its dispatches; disabling AO skips AO and its prepass when neither SSS nor
DoF needs depth. With SSS/DoF enabled, the depth prepass remains necessary.
Alpha masks are honored by the normal/depth prepass. Bloom extends beyond
geometry as additive RGB while preserving its coverage; FXAA filters both
premultiplied color and coverage so object silhouettes are smoothed too.

Render performance reports GPU timestamps from one completed submission:
Shadows, AO/depth, material shading, Bloom and tone mapping (including SSS,
DoF and FXAA). CPU times are not mixed into these values. No separate SSS timing
is claimed because its diffusion runs inside the tone mapping pass. A device
without timestamp support displays an unavailable message.

## Verification

`preview_effect_settings` checks preset/override resolution, explicit Off,
zero intensity, unsupported effects, malformed values, project round trips,
legacy loading and unknown-field preservation without a GPU.

`preview_render`, `preview_render_high` and `preview_render_async` cover real
Vulkan output, toggle restoration, non-skin isolation, contact AO, queued frames
with different settings, small/odd extents and resource resize/reuse, in addition
to existing skinning, transparency, texture and background regressions. Tests run
with Vulkan validation and synchronization validation enabled.

Validated on AMD Radeon Vega 8 (RADV RAVEN): the Release/glslc build succeeds
and all 28 enabled CTest cases pass, including auto/high/async Preview rendering
and audio/video export. The existing `regression_test` is disabled by the
repository configuration and was not counted as executed. All 18 Preview shader
entry/format variants also compile with the CI-pinned DXC v1.9.2602.24; the
three auto/high/async Preview GPU suites also pass with those DXC shader
artifacts. The glslc artifacts were restored after this cross-compiler check.
Japanese and English panels were also visually inspected.

```sh
cmake --build --preset linux-release -j 3
ctest --test-dir build/linux-release --output-on-failure
```

## Technical references

- [Jimenez et al., GTAO and indirect occlusion](https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf): horizon slice integration.
- [Intel XeGTAO](https://github.com/GameTechDev/XeGTAO): reference for view-space reconstruction, horizon searches and analytic visibility. This implementation uses the existing depth/normal prepass rather than importing XeGTAO's depth hierarchy or temporal pipeline.
- [Three.js tone mapping](https://github.com/mrdoob/three.js/blob/dev/src/renderers/shaders/ShaderChunk/tonemapping_pars_fragment.glsl.js): AgX polynomial/linear-output conventions and neutral compression.
- [Khronos PBR Neutral](https://github.com/KhronosGroup/ToneMapping/tree/main/PBR_Neutral): neutral highlight compression.

The compact AgX approximation is not a complete implementation of Blender's
color management. Validate production looks against the intended output images,
not only the view-transform name.
