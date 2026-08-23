# mikumikudesu

MikuMikuDayo 1.20 の Linux ネイティブ移植ブランチです。既存の Windows/D3D12
ソースとデータを保持しつつ、`SDL3 + Vulkan 1.3 + HLSL/SPIR-V` の独立した実行系を
追加しています。

## 現在動くもの

- Linux x86_64 上の SDL3 ウィンドウ、マウス・キーボード・ゲームパッド初期化
- SDL3 のファイル D&D（PMX/VMD/VPD、画像、音声、動画、`.dayo`、`.fxdayo` を分類）
- Vulkan 1.3 の instance/device/surface/swapchain、dynamic rendering、同期、resize
- Dear ImGui の `imgui_impl_sdl3 + imgui_impl_vulkan`
- HLSL から SPIR-V へのビルド（DXC優先、Previewのみglslcフォールバック可）
- PMX 2.0/2.1 のUTF-8/UTF-16メタデータ、頂点、法線、UV、BDEF/SDEF/QDEF、
  1/2/4 byteインデックスの読み込み
- PMXメッシュのVulkan頂点／インデックスバッファへのアップロードと静的Preview描画
- GPU機能ベースの Preview/Subayai/BDPT 判定とPreviewへの安全なフォールバック
- OIDNは HIP → CPU の順で実行時選択（OIDN自体は任意依存）
- FFmpeg開発ライブラリの任意検出
- `DEBUG/INFO → stdout`、`WARN/ERROR → stderr` のログ出力

現時点のネイティブPreviewは静的メッシュ確認用です。元アプリのVMD/VPDアニメーション、
Bullet変形、材質・テクスチャ、`.fxdayo`パスグラフ、Subayai/BDPTのシェーダー本体、
録音・動画デコード／書き出しは、Windows側ソースには存在しますが新しいVulkan実行系には
まだ接続されていません。RT拡張を持つGPUで機能判定が成功しても、現在表示する内容は
Previewメッシュです。この制約を隠して「Linux完全対応」とは扱いません。

## 必要環境

最小構成:

- Linux x86_64
- CMake 3.25以上、Ninja、C++20コンパイラ
- SDL 3.2以上
- Vulkan loaderとVulkan 1.3対応ドライバ
- glslc、またはDXC

AMD推奨構成:

- AMD Radeon RX 6000（RDNA2）以降
- Mesa RADV
- RAM 16 GB以上、VRAM 8 GB以上（BDPT移植完了後の推奨値）

PreviewだけならハードウェアRTは不要です。Subayai/BDPT向けには次を起動時に確認します。

- `VK_KHR_acceleration_structure`
- `VK_KHR_ray_tracing_pipeline`
- `VK_KHR_ray_query`
- `VK_KHR_fragment_shader_barycentric`
- buffer device address
- descriptor indexing

### Artix / Arch Linux

```bash
sudo pacman -S --needed \
  base-devel cmake ninja sdl3 \
  vulkan-headers vulkan-icd-loader vulkan-radeon vulkan-tools \
  vulkan-validation-layers shaderc
```

DXC、Bullet、OIDN、Jsonnet、DirectXTex等はディストリビューションのパッケージ、または
下記vcpkgマニフェストを利用できます。VulkanヘッダーとDear ImGuiが未導入の場合、通常の
CMakeプリセットは公式ソースの固定タグを自動取得します。

## ビルドと実行

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug

./build/linux-debug/mikumikudesu
./build/linux-debug/mikumikudesu --asset MikuMikuDayo/sample/deformTutorial.pmx
```

主なオプション:

```text
--renderer preview|subayai|bdpt  要求するレンダラー（不足機能時はPreview）
--asset PATH                     起動時にアセットを読む（複数指定可）
--probe                          GPU機能をJSONで表示して終了
--hidden --frames N              非表示ウィンドウでNフレーム描画するsmoke test
--no-validation                 Vulkan validationを要求しない
```

ネットワーク取得を禁止して、インストール済み依存だけを使う場合:

```bash
cmake --preset linux-system-only
cmake --build --preset linux-system-only
```

一括診断は次で実行できます。

```bash
./scripts/check-linux-amd.sh
```

## vcpkg

`vcpkg.json` には SDL3、Vulkan、DXC、DirectXTex（JPEG/PNG有効）、Bullet、
Dear ImGui、ImGuizmo、cereal、Jsonnetを宣言しています。

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset linux-release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build --preset linux-release
```

OIDNには現行vcpkg公式レジストリのportがないため、ディストリビューションまたはOIDN公式の
CMake installを利用します。DirectXTexのLinux版はDDS/HDR/TGAに加え、マニフェストの`jpeg`/`png` featureで
JPEG/PNG補助関数を有効にします。ネイティブPreviewの現在の色付き法線表示はまだ
DirectXTexを呼ばず、材質移植時の依存として準備しています。

## シェーダー

`cmake/CompileHlsl.cmake` は `dxc` を検出すると次に相当する設定を使います。

```bash
dxc -spirv -fspv-target-env=vulkan1.3 -fvk-use-dx-layout \
  -E VS -T vs_6_6 shader.hlsl -Fo shader.spv
```

DXCが無い場合は、移植基盤の単純なPreviewシェーダーに限ってglslcのHLSL frontendへ
フォールバックします。既存のbindless、ray query、ray tracingシェーダーを移す際はDXCを
必須にします。`SV_Barycentrics` はSPIR-Vのfragment barycentricへ写像する前提です。

## OIDN

OIDNは必須ではありません。ビルド時に見つかった場合、起動時にHIP deviceを試し、
利用できなければCPU deviceへフォールバックします。OIDNが無い場合もアプリは起動し、
denoiseだけを無効にします。HIPを使う環境では対応するROCm/HIP runtimeも別途必要です。

## Wine + vkd3d-protonの事前確認

配布物の `MikuMikuDayo/MikuMikuDayo.exe` はネイティブ移植とは別に、Wine prefixを一時
ディレクトリへ隔離して確認できます。

```bash
./scripts/wine-smoke.sh
```

これはD3D12→vkd3d-proton→Vulkan経路の互換性確認であり、Linuxネイティブ対応を意味しません。

## 構成

```text
src/
├── app/                     CLI、D&D、ImGui画面、メインループ
├── core/                    アセット分類、PMX、OIDN選択、ログ
├── platform/                SDL3 window/event/audio初期化
└── graphics/
    ├── device.hpp           backend非依存のdevice/resource/command契約
    └── vulkan/              Vulkan device/swapchain/pipeline/resource実装

MikuMikuDayo/src/            元のWin32/D3D12実装（比較・段階移植用）
MikuMikuDayo/hlsl/           既存HLSL
MikuMikuDayo/renderer/       Preview/Subayai/BDPT effectと素材
```

Vulkan/D3Dの座標差は `GraphicsConvention` とVulkan backend内で吸収し、CoreへAPI差を
持ち込みません。D3D12側を同じinterfaceへ接続する作業が完了するまでは、Windowsでは元の
配布ビルドを使用します。

## このAMD環境での確認結果

2026-08-24、AMD Ryzen 5 PRO 3500U / Radeon Vega 8 / Mesa RADV 26.1.7で確認:

- Vulkan API 1.4.354、Preview: 対応
- buffer device address / descriptor indexing: 対応
- acceleration structure / ray query / RT pipeline / fragment barycentric: 非対応
- core test、Vulkan probe: 成功
- Dear ImGui有効ビルド: 成功
- 同梱PMX（4,694頂点、7,860三角形）のアップロードと複数フレーム描画: 成功
- Subayai/BDPT要求時: 不足機能を列挙してPreviewへフォールバック

Vega 8はRDNA2ではないため、このRT非対応は想定どおりです。
