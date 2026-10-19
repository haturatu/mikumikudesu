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
| Depth of field | Thin-lens signed CoC; half-resolution near/far blur and coverage composite | F-number, animated face/eyes/bone and surface-click focus; perspective camera |

Skin diffuse is separate from specular highlights and hair, so diffusion does
not smear the final scene image. Transparent overlays and outlines erase the
skin mask where they draw; this conservatively leaves these boundaries sharp.
The low-cost transmission approximation also remains useful for backlit skin.
Screen-space effects cannot recover hidden or off-screen geometry. SSR, SSGI,
TAA, refractive glass and polygonal aperture bokeh remain separate work.

The previous-frame background mode uses the direct shading path. Color
management and material lighting still apply, but HDR postprocessing is bypassed
in that mode. Normal previews and exported frames use the HDR path.

## Lens and focus

Enable **Depth of field**, then open **Lens and focus**. Aperture is an F-number:
opening from f/5.6 to f/1.4 multiplies the uncapped blur radius by four.
**Camera target** retains the existing focus behavior; **Manual distance** uses
metres; **Bone**, **Face** and **Eyes** follow the selected model's evaluated
animation. Eyes uses the left/right eye midpoint, then available eye/head bone
aliases. Face also prefers eye locations and falls back to the head. Missing
models/bones fall back to the camera target and show a message.
**Focus selected bone** uses the bone currently selected in the Inspector.
Model references are mapped through the project model source when asset loading
order differs from metadata order. A missing model keeps its saved reference
instead of focusing on whichever model loaded into its old slot.
Neither focus mode changes the camera pose or a VMD camera track.

Ctrl + left click on a viewport surface selects **Viewport point** and enables
DoF. The selected 3D point stays fixed while the camera moves. A drag with Ctrl
held at its start never becomes an orbit gesture. Picking works while VMD camera
follow locks navigation. UI coordinates are normalized against the displayed
image and mapped to the actual framebuffer, including DPI scaling. The GPU
copies one D32 pixel to a frame-owned coherent staging buffer. Results are
polled after the frame fence signals, without a device-wide wait; unprojection
uses that submission's camera and pixel centre. New requests, changing focus
controls and loading/resetting projects invalidate older picks. Clear depth is
ignored. The depth prepass includes opaque/alpha-tested fragments with combined
alpha >= 0.98; picking sees the opaque surface behind blended materials.

**Focus offset** moves the focal plane along the camera axis in metres. The UI
shows the resolved distance including this offset. Lens calibration exposes
sensor height (default 24 mm) and metres per preview unit (default 0.8). Preview
normalization scales each model's largest dimension to 1.8 units. For a character
whose largest dimension is height, set metres/unit to intended height / 1.8;
for example 0.889 gives approximately 1.6 m. This calibration is explicit because
PMX files do not specify metres. Existing per-model normalization is preserved;
this is a virtual scene scale, not recovery of absolute PMX physical dimensions.
**Reset lens and focus** restores the lens defaults separately from effect
intensity/preset overrides.

The vertical FOV determines focal length: `f = sensorHeight / (2 tan(FOV / 2))`.
All optical distances are converted to metres. The signed full-resolution CoC
radius is

```text
r = f² / (2 N (s - f)) * (s / z - 1) * imageHeight / sensorHeight
```

`N` is the F-number, `s` the axial focus distance and `z` reconstructed view Z.
Positive CoC is near, negative is far, and the focal plane is zero. The virtual
lens is suspended during orthographic VMD keys; SSS remains available. The UI
explains the perspective requirement. The previous-frame viewport background
still bypasses HDR postprocessing; choose White, Image or Video to preview DoF.
Normal exports use the same HDR/DoF implementation.

A synthetic red foreground / blue background comparison from the GPU fixture:

| f/32, focus 2.4 m | f/0.7, focus 2.4 m | f/0.7, focus 0.8 m |
| --- | --- | --- |
| ![Closed aperture](dof-closed.png) | ![Foreground blur expands](dof-open.png) | ![Foreground in focus](dof-focused.png) |

## Spatial processing pipeline

```text
Depth / normal prepass -> HDR color + skin diffuse
  -> full-resolution SSS (once per pixel)
  -> full-resolution signed CoC
  -> depth-compatible 2x2 near/far downsample
  -> 8x8 half-resolution tile ranges + radius dilation
  -> half-resolution near / far disk gathers
  -> full-resolution composite -> Bloom -> tone mapping + FXAA
```

CoC uses sampled/storage R16_SFLOAT when the device enables extended storage
formats and supports that format, otherwise R32_SFLOAT. SSS and DoF intermediate
colors are RGBA16_SFLOAT. Each in-flight frame owns its images, descriptors and
readback buffer; a completed slot can resize/reallocate without changing another
submission. With DoF disabled, SSS allocates one full-resolution output and only
dummy images for unused descriptors. With both disabled, spatial dispatches are
skipped. Empty tiles skip disk sampling. No subgroup, RDNA-specific or FP16
arithmetic instructions are required.

Near gathers test each source disk's radius against the receiving pixel, so
foreground coverage can extend beyond the original silhouette. Premultiplied
RGB and alpha travel together. Far downsampling/gather and upsampling reject
incompatible depth; near coverage deliberately crosses depth boundaries.
Near is composed over the far result. Blur activation fades smoothly between
0.5 and 2 full-resolution pixels so subpixel CoC retains the sharp image.
Hidden background is approximated from available far samples; off-screen or
occluded detail and the depth of blended glass/hair cannot be reconstructed
from a single opaque depth layer. Aperture shape is a disk, not polygonal bokeh.

| DoF quality | Resolution (both axes) | Disk samples | Radius cap at 1080p |
| --- | --- | --- | --- |
| Performance | 1/2 | 12 | 20 px |
| Balanced | 1/2 | 24 | 32 px |
| High | 1/2 | 32 | 48 px |

Auto follows the effective device/override preset. Radius caps scale with image
height; changing viewport resolution does not change the normalized blur size.
Smaller F-numbers stop increasing blur after the selected cap is reached.
The legacy intensity remains an optional artistic CoC multiplier; use F-number
for lens control.

For the included 1080p synthetic scene (focused foreground rectangle, defocused
background), median warmed DoF GPU times on Radeon Vega 8 / RADV RAVEN with
validation enabled were:

| Quality | CoC | Downsample / tiles | Near / far | Composite | DoF sum |
| --- | --- | --- | --- | --- | --- |
| Performance | 1.04 ms | 2.79 ms | 4.29 ms | 5.06 ms | 13.18 ms |
| Balanced | 1.04 ms | 2.78 ms | 8.19 ms | 5.06 ms | 17.07 ms |
| High | 1.04 ms | 2.82 ms | 10.93 ms | 5.02 ms | 19.81 ms |

These figures exclude scene rendering, SSS, Bloom and tone mapping. They are
measurements of this fixture, not a whole-frame or complex-model performance
promise. Start with Performance on an APU; opening the lens does not remove the
selected blur-radius cap.

## Runtime behavior

Switches reuse compiled shaders and pipelines. Effect constants use a new
storage buffer whose old allocation is retired behind frame fences, so GUI
changes do not call `vkDeviceWaitIdle()` or overwrite in-flight uniforms. Bloom
images and tone mapping descriptors belong to each frame slot. Disabled Bloom
skips its dispatches; disabling AO skips AO and its prepass when neither SSS nor
DoF or a surface pick needs depth. With SSS/DoF enabled, the depth prepass remains necessary.
Alpha masks are honored by the normal/depth prepass. Bloom extends beyond
geometry as additive RGB while preserving its coverage; FXAA filters both
premultiplied color and coverage so object silhouettes are smoothed too.

Render performance reports GPU timestamps from one completed submission:
Shadows, AO/depth, material shading, SSS, CoC, downsample/tiles, near/far blur,
DoF composite, Bloom and tone mapping/FXAA. CPU times are not mixed into these
values. A device without timestamp support displays an unavailable message.

## Verification

`preview_effect_settings` checks preset/override resolution, explicit Off,
zero intensity, unsupported effects, malformed values, project round trips,
legacy loading and unknown-field preservation without a GPU. It also checks
F-number/radius scaling, signed CoC, rotated camera unprojection, animated bone
positions and eye/head fallbacks. Lens persistence and malformed fields are
covered by the same suite; it also passes Clang ASan/UBSan.

`preview_render`, `preview_render_high` and `preview_render_async` cover real
Vulkan output, toggle restoration, non-skin isolation, contact AO, queued frames
with different settings, small/odd extents and resource resize/reuse, in addition
to existing skinning, transparency, texture and background regressions. Tests run
with Vulkan validation and synchronization validation enabled. DoF-specific
checks cover aperture response, foreground coverage outside its silhouette,
focused foreground isolation, per-pass timings, all quality modes, tiny/odd
extents, orthographic suspension, asynchronous picking with camera changes,
opaque-surface selection behind blending, sky rejection and cancellation.
`viewport_interaction` verifies Ctrl-click UVs, one-shot delivery and navigation
capture while VMD follow is active.

Validated on AMD Radeon Vega 8 (RADV RAVEN): the Release/glslc build succeeds
and all 29 enabled CTest cases pass, including auto/high/async Preview rendering
and audio/video export. The existing `regression_test` is disabled by the
repository configuration and was not counted as executed. All 20 Preview shader
entry/format variants also compile with the CI-pinned DXC v1.9.2602.24; the
three auto/high/async Preview GPU suites also pass with those DXC shader
artifacts. The glslc artifacts were restored after this cross-compiler check.
Japanese and English panels were also visually inspected.

```sh
cmake --build --preset linux-release -j 3
ctest --test-dir build/linux-release --output-on-failure
```

To run the optional 1080p synthetic-scene timing fixture (after warming the
frame slots), use `DAYO_TEST_DOF_BENCHMARK=1 ctest --test-dir build/linux-release -R '^preview_render$' -V`. `DAYO_TEST_DOF_OUTPUT=/tmp/dof-review` saves aperture
and focal-plane comparison PNGs. Timings are workload and clock dependent;
these presets do not promise a frame rate on every Vega 8 system.

## Technical references

- [AMD FidelityFX DoF 1.1](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/depth-of-field/): thin-lens CoC, independent near/far processing and reduced-resolution reconstruction. The compact shader here is independently implemented; FidelityFX SDK code is not included.
- [GPU Gems 3, Practical Post-Process Depth of Field](https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-28-practical-post-process-depth-field): foreground silhouette expansion.
- [Vulkan image copies](https://docs.vulkan.org/guide/latest/image_copies.html): depth image-to-buffer copy constraints.
- [Jimenez et al., GTAO and indirect occlusion](https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf): horizon slice integration.
- [Intel XeGTAO](https://github.com/GameTechDev/XeGTAO): reference for view-space reconstruction, horizon searches and analytic visibility. This implementation uses the existing depth/normal prepass rather than importing XeGTAO's depth hierarchy or temporal pipeline.
- [Three.js tone mapping](https://github.com/mrdoob/three.js/blob/dev/src/renderers/shaders/ShaderChunk/tonemapping_pars_fragment.glsl.js): AgX polynomial/linear-output conventions and neutral compression.
- [Khronos PBR Neutral](https://github.com/KhronosGroup/ToneMapping/tree/main/PBR_Neutral): neutral highlight compression.

The compact AgX approximation is not a complete implementation of Blender's
color management. Validate production looks against the intended output images,
not only the view-transform name.
