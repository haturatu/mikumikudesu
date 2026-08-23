# mikumikudesu

MikuMikuDayo 1.20をLinuxへ移植したネイティブ実行系です。元のWindows/D3D12ソースと
アセットを保持しつつ、Linux側を`SDL3 + Vulkan 1.3 + HLSL/SPIR-V`で構成しています。

## 実装済み

- SDL3ウィンドウ、HiDPI/resize、ファイルD&D、右ドラッグカメラ、ホイールズーム
- Vulkan 1.3 swapchain、dynamic rendering、同期、深度、αブレンド、材質別両面描画
- Dear ImGuiのSDL3/Vulkan backend
- PMX 2.0/2.1の全セクション（頂点、材質、ボーン/IK、全モーフ、表示枠、剛体、
  ジョイント、soft-bodyデータ）の検証付き読み込み
- BDEF1/2/4、SDEF、QDEF、VMDベジェ補間、VPD、ボーン継承、CCD IK、全PMXモーフ
- Bullet剛体/6DoF spring、collision group、固定step、物理ボーンへの往復反映
- PNG/JPEG/BMP/TGA/HDRとDDS（RGBA/BGRA、BC1～BC5）の読込、Vulkan sRGB texture upload
- PMX材質範囲、diffuse/ambient/specular/power、texture乗算/加算モーフ、VMDカメラ/照明
- FFmpegによるWAV/MP3/M4A等の音声再生とMP4/AVI/MKV/MOV/WebM動画デコード
- Jsonnetを実行した`.fxdayo`のtexture/sampler/pass/raster/compute/raytracing graph解析
- `.dayo` v2の相対パス保存と原子的置換、v2再読込、旧版JSONヘッダーからのasset復元
- Vulkan feature単位のPreview/Subayai/BDPT判定と、安全なPreviewフォールバック
- OIDNのHIP→CPU runtime選択（OIDNは任意依存）
- `DEBUG/INFO → stdout`、`WARN/ERROR → stderr`のログ規約

## 制約

PreviewはLinux/AMDで実動します。SubayaiとBDPTについては、`.fxdayo`グラフと必要featureの
検出までは移植済みですが、Vulkan acceleration structure/SBTと各passの実行器は未接続です。
そのため`nativeSubayai`と`nativeBdpt`は意図的にfalseであり、対応していると偽装しません。

また、PMX 2.1 soft-bodyは安全に解析しますが、現在のBullet worldではsimulation対象外です。
旧Windows `.dayo`のJSON asset情報は読めますが、後続する独自binary keyframe streamはまだ
読みません。複数モデル編集、ImGuizmo編集、動画書き出しも元Windows UI相当には未到達です。

## 必要環境

- Linux x86_64
- CMake 3.25以上、Ninja、C++20コンパイラ
- SDL 3.2以上
- Vulkan loaderとVulkan 1.3対応ドライバ
- glslc、またはDXC
- Preview: Vulkan swapchain対応GPU
- RT実装の開発・検証: RDNA2/RX 6000以降など、起動時に表示するRT featureを満たすGPU

AMDではMesa RADVを推奨します。BDPTを将来有効化する構成ではRAM 16GB、VRAM 8GB以上を
推奨します。

確認するRT feature:

- `VK_KHR_acceleration_structure`
- `VK_KHR_ray_tracing_pipeline`
- `VK_KHR_ray_query`
- `VK_KHR_fragment_shader_barycentric`
- buffer device address / descriptor indexing

### Artix / Arch Linux

```bash
sudo pacman -S --needed \
  base-devel cmake ninja sdl3 ffmpeg bullet nlohmann-json \
  vulkan-headers vulkan-icd-loader vulkan-radeon vulkan-tools \
  vulkan-validation-layers shaderc
```

不足するVulkan-Headers、stb、Dear ImGui、Bullet、Jsonnet等は、通常presetでは固定versionを
CMakeが取得します。取得を禁止する場合は必要なdevelopment packageを先に導入してください。

## ビルドと実行

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug --output-on-failure

./build/linux-debug/mikumikudesu
./build/linux-debug/mikumikudesu --asset model.pmx --asset motion.vmd
```

主なオプション:

```text
--renderer preview|subayai|bdpt  要求renderer（利用不可なら理由を表示してPreview）
--asset PATH                     起動時asset。複数指定可
--save-project PATH              読み込んだassetと状態を.dayoへ保存
--probe                          GPU featureをJSONで出力して終了
--hidden --frames N              非表示windowでN frame描画するsmoke test
--no-validation                 Vulkan validationを要求しない
```

system packageのみで構成する場合:

```bash
cmake --preset linux-system-only
cmake --build --preset linux-system-only
ctest --preset linux-system-only --output-on-failure
```

AMD環境の一括診断:

```bash
./scripts/check-linux-amd.sh
```

このスクリプトはGPU probe、unit test、PMX+VMD+Bullet、画像、`.dayo` round trip、
FFmpeg動画/音声（CLIがある場合）を実際に実行します。

## シェーダーと依存

`cmake/CompileHlsl.cmake`はDXCがあればVulkan 1.3向けSPIR-Vを生成し、単純なPreviewのみ
glslc HLSL frontendへfallbackします。既存のbindless/ray query/ray tracing shaderを移す場合は
DXCが必須です。`SV_Barycentrics`はSPIR-V fragment barycentricへ写像する設計です。

`vcpkg.json`にはSDL3、Vulkan、DXC、DirectXTex、Bullet、Dear ImGui、ImGuizmo、cereal、
Jsonnetを宣言しています。Linux実行系の画像読込は現在stbと内蔵DDS decoderを使うため、
WICには依存しません。OIDNは任意で、HIP deviceが使えなければCPUへfallbackします。

## 構成

```text
src/
├── app/                     CLI、D&D、ImGui、main loop、project連携
├── core/                    PMX/VMD/VPD、animation、Bullet、media、effect、project
├── platform/                SDL3 window/event/audio
└── graphics/
    ├── device.hpp           API非依存device/resource契約
    └── vulkan/              Vulkan swapchain/pipeline/resource実装

MikuMikuDayo/src/            元のWin32/D3D12実装（比較用）
MikuMikuDayo/hlsl/           既存HLSL
MikuMikuDayo/renderer/       Preview/Subayai/BDPT effectと素材
```

## このAMD環境での確認結果

2026-08-24、AMD Ryzen 5 PRO 3500U / Radeon Vega 8 / Mesa RADV 26.1.7:

- Vulkan API 1.4.354、Preview対応
- buffer device address / descriptor indexing対応
- acceleration structure / ray query / RT pipeline / fragment barycentric非対応
- unit test、Vulkan probe、ImGui build成功
- 同梱PMX（4,694頂点、7,860三角形）+ VMD + Bulletを10 frame描画成功
- PNG表示、FFmpeg動画/音声、`.dayo`保存/再読込成功
- Subayai/BDPT要求時は不足featureを列挙しPreviewへfallback

Vega 8はRDNA2ではないため、RT実行試験はハードウェア上不可能です。
