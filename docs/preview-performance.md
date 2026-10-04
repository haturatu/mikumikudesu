# Preview performance on integrated AMD GPUs

Previewの改善はGPU帯域、頂点変形、フレーム同期を中心に実装しています。既存のVulkan 1.3、
VMA、upload ring、bindless、pipeline cache、ThinLTO/PGO/BOLTをそのまま利用します。

## 変更した処理

- AO: scalar R8_UNORM。storage、sampling、linear filteringとextended storage formats featureを確認します。
  未対応ならR16_SFLOAT、extended formats自体が未対応ならR32_SFLOATへfallbackします。
  `high`では対応時にR16Fを選びます。shaderも選んだstorage image formatに一致するvariantを使います。
- Normal: octahedral encodingのRG16_SNORM。未対応ならRG16F、最後にRGBA16Fへfallbackします。
  AOではencoded normalをpoint fetchしてから復号し、octahedronの継ぎ目の誤補間を避けます。
- Morph + BDEF/SDEF/QDEF: 64 threadsのPreviewDeformを1フレームに1回dispatch。
  36 byte/vertexのposition、normal、UV、edge scaleをShadow、Normal、Main、Edgeが共有します。
  glslcでも動くようPreviewで使っていたskin計算をcomputeへ移し、バッファのbyte配置を明示しています。
  上流Dayo依存のnative_deformとPreviewの挙動は別々に維持します。
- CPU更新: canonical vertices、bones、morph、materials、indirectを保持し、current slotのfence後にコピー。
  材質はslotごとに未反映dirty rangeを累積します。他slotのmapped bufferからコピーしません。
- 破棄: メッシュ、材質descriptor、テクスチャ、背景、SH、offscreen、typed resourcesをtimelineでretire。
  graphicsとexperimental compute両方の完了を確認します。shutdownとswapchain再作成では待機が残ります。
- Readback: 3本のpersistently mapped bufferとtimeline ticket。
  CLI/GUIのMP4出力は描画をenqueueし、古い完成フレームを順番にencoderへ渡します。
  単発の`renderToImage()`は同じAPIの同期wrapperです。サイズ変更中も古いticketの寸法を保持します。
- Shadow: comparison sampler + SampleCmpLevelZero。D32のlinear filteringがあればhardware PCF、
  なければnearest comparison。固定Preview samplerはimmutableです。
- HDRI: prefilter sample数はmipごとに1/64/48/32/16。
  linear pixels、exposure適用後のpixels、source/cube寸法、shader source digestとsample scheduleをkeyにし、
  `$XDG_CACHE_HOME/mikumikudesu/environment/`（未設定なら`$HOME/.cache/`以下）へcubeと全prefilter mipを保存します。
  初回bakeを置換・解放するときにreadbackして保存するため、その初回保存にはGPU待機があります。
  次回はcomputeを省略してcached facesをuploadします。checksum/寸法/長さが不一致なら再計算します。

1080p・半解像度AOの理論上のattachment payloadは、従来の約23.73 MiBからR8 AO + RG16 normalで
約8.90 MiBへ減ります（約62.5%削減）。allocation padding、depth、HDR合成、compute vertex出力は別です。
この数字はFPSの改善率を表しません。

## Quality / presentation

| 設定 | Shadow | AO | Environment上限 | realtime shadow taps |
| --- | --- | --- | --- | --- |
| auto: discrete / 十分なbudget | 2048² | half R8 | 512² | 3×3 comparison |
| auto: integrated / apu | 1024² | half R8 | 256² | 1 comparison（linear時2×2 PCF） |
| auto: available 512 MiB未満 | 1024² | quarter R8 | 128² | 1 comparison |
| high | 2048² | half R16F | 512² | 3×3 comparison |
| still quality | 2048² | 選択profileのAO | 選択profileの上限 | 7×7 comparison |

`VK_EXT_memory_budget`があればVMAのbudget flagを有効にします。autoでは120フレームごとに
DEVICE_LOCAL heapのbudget−usageを確認し、512 MiB/1 GiBの閾値で品質を下げます。
同じsession内では自動で上げず、閾値付近でresourceを作り直し続けることを避けます。
Environment上限の変更は次回のHDRI再生成時に反映されます。拡張なし/VMAなしはdevice-based profileです。

```bash
./build/linux-release/mikumikudesu --preview-quality apu --asset model.pmx
./build/linux-release/mikumikudesu --preview-quality high --asset model.pmx
./build/linux-release/mikumikudesu --present immediate --no-validation --asset model.pmx
```

`--present fifo|fifo-relaxed|mailbox|immediate`の既定はFIFO。surfaceが要求modeに対応しなければ
WARNをstderrへ出してFIFOへ戻ります。MAILBOXでは可能なら3枚のswapchain imageを確保します。
Offscreen出力はPresent modeの影響を受けません。

## Async compute A/B

```bash
./build/linux-release/mikumikudesu --async-compute --present immediate --asset model.pmx
```

既定は無効です。compute-only familyがあればそのqueue、なければgraphics familyの第2queueを使います。
未対応ならserialへfallbackします。別familyではPreviewのsource/output bufferをconcurrent sharingとし、
専用timelineからgraphicsのvertex inputへ依存を渡します。現時点でoffloadするのはPreviewDeformです。
SSAOとenvironmentはgraphics queue上です。SSAOとShadowのoverlapにはpass分割・追加の測定が必要です。
APUのCU/メモリ帯域は共有されるため、有効化だけで高速化したとは判断しません。

## 測定とドライバー

GPU/driver、モデル、motion、解像度、profile、cacheのcold/warm状態、validationの有無を揃えます。
CPUビルドや他のGPU処理を止め、通常設定とasyncの順序を交互にして複数回測ります。
動画出力のwall timeにはencoder、readback、モデル読み込みも含まれるため、interactive FPSとは区別します。

Mesaの環境変数はアプリが変更しません。通常は追加設定なしで使います。
cache調査時のみ以下のように統計を出します。

```bash
MESA_SHADER_CACHE_SHOW_STATS=true ./build/linux-release/mikumikudesu --present immediate --asset model.pmx
RADV_PERFTEST=nircache MESA_SHADER_CACHE_SHOW_STATS=true ./build/linux-release/mikumikudesu --asset model.pmx
```

nircacheはpipeline作成時間の比較用です。Vega/GFX9にGFX10+用wave32設定を推奨しません。
既存のnative tuning、ThinLTO/PGO/BOLTはCPU workload向けのopt-in設定を利用してください。

## Vega 8 / RADV 26.2.4での実測

2026-10-04、AMD Radeon Vega 8 Graphics (RADV RAVEN)、Mesa 26.2.4-arch1.1、GCC 16.2.1、
Release・glslc build。比較元はmainの`d9e36b5`、同じ依存関係・UI有効のRelease buildです。
Teto PMX（31,954 vertices / 52,710 triangles）、VMDなし、既定の物理設定、timeline 0–59、
1920×1080、30 fps、software H.264、音声なし、validationなしでCLI MP4を出力しました。
各条件1回warmup後、実行順を交互にして3回計測し、全出力の60 frames・寸法をffprobeで確認しました。

| 条件 | 3回のwall time（秒） | 中央値（秒） | main比の時間短縮 |
| --- | --- | --- | --- |
| main d9e36b5 | 7.520 / 7.688 / 7.439 | 7.520 | — |
| high | 6.933 / 6.680 / 6.843 | 6.843 | 9.0% |
| auto (APU) | 6.945 / 6.738 / 6.709 | 6.738 | 10.4% |
| auto + async compute | 6.791 / 6.814 / 6.652 | 6.791 | 9.7% |

ロード・初期化・CPU encode込みの単一workloadです。描画だけのGPU時間、interactive FPS、
全モデルでの改善率は未測定です。autoとhighはShadow/AO精度も異なります。
asyncはこの条件でserial autoを上回らず、既定では無効にしています。

検証: ReleaseのCTest 21件（既存で無効のupstream regression_testは除外）。
通常・high・asyncの描画比較はVulkan synchronization validationを有効にして通過しました。
SDEF/QDEF・ゼロ法線・輪郭・背景・複数材質・texture format roundtrip・リング満杯/重複ticket・
未回収frame中のresource拡張とサイズ変更を含みます。Vegaで未対応のD24S8 fixtureはfeature queryでskipします。
VMAなし・ImGuiなしのsystem-only buildも成功し、通常・high・asyncの描画テストは通過しました。
この構成のCTestは19件通過、2件（fx_executor / upstream_compat）は無効化されているJsonnetを
必要とするため失敗しています。DXCがこの環境にないため、DXC compilerによるbuildは未検証です。

参考: [Mesa environment variables](https://docs.mesa3d.org/envvars.html)、
[Vulkan format requirements](https://docs.vulkan.org/spec/latest/chapters/formats.html)、
[Vulkan synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)、
[VMA initialization](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/group__group__init.html)。
