# mikumikudesu

Windows環境が無いのでただの `GNU/Linux` + AMD 環境でどうしても動かしたい為だけのプロジェクト。
  
MikuMikuDayo 1.30を初回セットアップ時に取得して使用するLinux向けネイティブ実行系です。
Linux側を`SDL3 + Vulkan 1.3 + HLSL/SPIR-V`で構成しています。

本アプリのバージョンは`0.1.0`、upstream互換対象は`1.30`として独立して管理します。
1.30のassetを基準にしていますが、全FX・solverの完全互換を意味しません。
対応段階と残件は[1.30互換性](docs/upstream-1.30.md)を参照してください。

## 実装済み

- SDL3ウィンドウ、HiDPI/resize、ファイルD&D、右ドラッグカメラ、ホイールズーム
- Vulkan 1.3 swapchain、dynamic rendering、同期、深度、opaque/transparent pass、材質別両面描画
- Dear ImGuiのSDL3/Vulkan backend
- PMX 2.0/2.1の全セクション（頂点、材質、ボーン/IK、全モーフ、表示枠、剛体、
  ジョイント、soft-bodyデータ）の検証付き読み込み
- BDEF1/2/4、SDEF、QDEF、VMDベジェ補間、VPD、ボーン継承、CCD IK、全PMXモーフ
- Bullet剛体/6DoF spring、collision group、固定step、物理ボーンへの往復反映
- PNG/JPEG/BMP/TGA/HDRとDDS（RGBA/BGRA、BC1～BC5）の読込、Vulkan sRGB texture upload
- PMX材質範囲、diffuse/ambient/specular/power、Toon/Sphere map、texture乗算/加算モーフ、VMDカメラ/照明
- Original Preview parity: BDEF1/2/4、SDEF、QDEF/DQSのGPUスキニング、VMD LightColor、α=0 discard、
  α>=0.98のopaque化、PMX材質順描画、共有Toon/Sphere map
- Preview debug tools: PMX材質インスペクタ、材質単体表示、texture/sphere/toon無効化、UV/normal可視化、
  オプションのscreen-space PMX outline
- FFmpegによるWAV/MP3/M4A等の音声再生とMP4/AVI/MKV/MOV/WebM動画デコード
- FFmpegを使ったストリーミングAAC/M4A音声書き出し（CLI / 非同期ImGui UI）
- Vulkan Previewのオフスクリーンreadbackと、決定論的なタイムラインでのMP4動画書き出し（H.264/H.265/AV1 + AAC）
- Jsonnetを実行した`.fxdayo`のtexture/sampler/pass/raster/compute/raytracing graph解析
- 複数PMXを保持できるScene（モデル別VMD/VPD/物理、表示切替、clone、背景画像/動画/音声の共存）
- `.dayo` v1/v2互換読込、Dayo 1.30 `.dayo` v3の複数subset読込/原子的保存、公式VMdayo v3の双方向変換と未知payload保持
- VMD Bézier/Linear/Catmull-Rom、外部親リンクの検証（循環参照検出）、重力key評価
- PMX 2.1 soft-bodyの決定論的フォールバックシミュレーション
- Undo/Redo CommandHistory、dirty flag/runtime mode、非同期連番フレーム出力（PPM/PNG）
- MaterialParameterBlock、effect render-graph compile、失敗時に旧状態を保持するFX hot reload
- Subayaiの`_template.txt`/材質注釈パーサと、髪材質のAnisotropy/IOR/AutoNormalデータ契約
- screen.bmpのPreviousFrame/BackgroundVideo/BackgroundImage/White semanticsを持つrenderer契約
- `.dayo` v2の相対パス保存と原子的置換、旧版assetとbinary keyframeの復元
- Vulkan feature単位のPreview/Subayai/BDPT判定と、安全なPreviewフォールバック
- OIDNのHIP→CPU runtime選択（OIDNは任意依存）
- `DEBUG/INFO → stdout`、`WARN/ERROR → stderr`のログ規約

## 制約

PreviewはLinux/AMDで実動します。SubayaiとBDPTについては、`.fxdayo`グラフと必要featureの
検出までは移植済みですが、Vulkan acceleration structure/SBTと各passの実行器は未接続です。
そのため`nativeSubayai`と`nativeBdpt`は意図的にfalseであり、対応していると偽装しません。

Subayaiの材質注釈はコア層で読み込めます。標準の`hair.txt`からは異方性、IOR、AutoNormalを
取得できますが、これらを実際のハイライトへ反映するnative Subayai passはまだ未接続です。

Subayai/BDPTのVulkan pass executor（acceleration structure/SBTを含む）は、featureのないAMD
Vega等でも起動できるよう未接続のままです。RT対応GPUではbackend契約とgraph compileまでを
検証し、未対応GPUでは不足featureを表示してPreviewへ戻します。OpenEXR encoderは任意依存の
ため現在は未接続です（連番出力はPNG/PPM）。
`.vmdayo`は本家1.30ソースのv3レイアウト（header、model dictionary、metadata、全track、
axis別MMD/Catmull-Rom方式）を実装しています。単体ファイルの二重headerと`.dayo`内の単一headerを
区別し、カメラsubsetとモデル別subsetを双方向変換します。未知の入力はopaque payloadとして保持します。
本家Windowsバイナリが生成したfixtureによる相互運用テストは、fixtureを同梱していないため未実施です。

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

### Linux release tuning

ローカルCPU向けのISA最適化は既定で無効です。対象マシン専用のreleaseを作る場合は、
`cmake --preset linux-release-native` を使えます。LLVM BOLTは通常のPGO収集後に、例えば
`-DDAYO_ENABLE_BOLT=ON -DDAYO_BOLT_PROFILE=/path/to/perf.fdata` を付けて有効化します。
BOLTのプロファイルは、対象バイナリを `perf record` で実行し、`perf2bolt` で変換して作成します。
例えば `perf2bolt ./build/linux-release-native/mikumikudesu -p perf.data -o perf.fdata`
のように生成します。
BOLT版は通常の実行ファイルとは別に `mikumikudesu.bolt` として生成され、BOLT有効時のinstallだけが
それを `mikumikudesu` として配置します。BOLTは対象環境専用の最終最適化として扱ってください。
プロファイル取得時とBOLT適用時は同じ `build/linux-release-native/mikumikudesu` を対象にし、
`--emit-relocs` を含む同じlink条件を使ってください。初回はBOLTを無効にしてrelocationを出力し、
プロファイルを取得してから、同じbuild treeでBOLTを有効にして再ビルドします。

```sh
cmake --preset linux-release-native \
  -DDAYO_ENABLE_BOLT=OFF -DCMAKE_EXE_LINKER_FLAGS=-Wl,--emit-relocs
cmake --build --preset linux-release-native
perf record --output=perf.data -- ./build/linux-release-native/mikumikudesu
perf2bolt ./build/linux-release-native/mikumikudesu -p perf.data -o perf.fdata
cmake --preset linux-release-native \
  -DDAYO_ENABLE_BOLT=ON -DDAYO_BOLT_PROFILE="$PWD/perf.fdata"
cmake --build --preset linux-release-native
```

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

MakefileからCMake Presetsを利用する簡易コマンドも用意しています。

```bash
# Debug（configure + build）
make

# Pinned MikuMikuDayo assetを取得してテスト
make setup
make test

# Test / run
make run
make run ARGS="--asset model.pmx --asset motion.vmd"

# Release / package / sanitizers / system packages only
make release
make package
make sanitize
make system
```

`make`/`make build`はMikuMikuDayoを取得しません。`make setup`、`make test`、`make install`、
`make package`でだけ、`deps/mikumikudayo.lock`に固定したGitHub Release ZIPを取得または
`.cache/mikumikudayo/`から再利用します。ZIPはSHA-256を検証してから展開します。

全ターゲットと変数は`make help`で確認できます。CMakeを直接実行する正式な手順も引き続き利用できます。

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
python3 scripts/fetch-mikumikudayo.py
ctest --preset linux-debug --output-on-failure

./build/linux-debug/mikumikudesu
./build/linux-debug/mikumikudesu --asset model.pmx --asset motion.vmd
```

### Linux native optimization profiles

Release builds can opt into Clang ThinLTO without changing the default presets:

```bash
cmake --preset linux-clang-thinlto
cmake --build --preset linux-clang-thinlto
```

For representative profile-guided optimization, first build and run the
profile-generation preset through a realistic workload. Clang writes
per-process `.profraw` files into the configured profile directory; merge them
before configuring the use build:

```bash
cmake --preset linux-pgo-generate
cmake --build --preset linux-pgo-generate
./build/linux-pgo-generate/mikumikudesu --asset model.pmx
cmake --build build/linux-pgo-generate --target dayo_pgo_merge
cmake --preset linux-pgo-use
cmake --build --preset linux-pgo-use
```

The Clang path uses `llvm-profdata merge` and `-fprofile-instr-use`; GCC keeps
its `-fprofile-use`/`-fprofile-correction` flow. PGO is intentionally opt-in
because profile data is workload-specific and must not be committed.

### ミク・テトのサンプルモデル

動作確認用として、配布元のアーカイブをローカルの`assets/models/`へ取得できます。
アーカイブは再配布しない前提で`.gitignore`対象にし、各モデルのREADMEとテクスチャは展開したまま保持しています。

- `assets/models/miku/model/miku.pmx` — Tda式初音ミク デフォ服ver（[BowlRoll #16344](https://bowlroll.net/file/16344)、作者: 金子卵黄/Tda。ReadMeの利用条件を優先）
- `assets/models/teto/model/teto.pmx` — Tda式重音テトTypeS（[BowlRoll #11308](https://bowlroll.net/file/11308)、作者: Tda/やまもと。ReadMeの利用条件を優先）

起動例:

```bash
./build/linux-debug/mikumikudesu --asset assets/models/miku/model/miku.pmx
./build/linux-debug/mikumikudesu --asset assets/models/teto/model/teto.pmx
# 複数モデル + motion + 背景を同一Sceneへ読み込むこともできます
./build/linux-debug/mikumikudesu \
  --asset assets/models/miku/model/miku.pmx \
  --asset assets/models/teto/model/teto.pmx \
  --asset motion.vmd --asset background.png
```

重音テトを使った作品の公開や改変時は、[重音テト公式ガイドライン](https://kasaneteto.jp/guidelines/)と各モデル付属READMEを確認してください。

主なオプション:

```text
--renderer preview|subayai|bdpt  要求renderer（利用不可なら理由を表示してPreview）
--asset PATH                     起動時asset。複数指定可
--save-project PATH              読み込んだassetと状態を.dayoへ保存
--probe                          GPU featureをJSONで出力して終了
--hidden --frames N              非表示windowでN frame描画するsmoke test
--no-validation                 Vulkan validationを要求しない
--export-m4a PATH               Vulkanなしで音声をM4A/AACへ書き出す
--audio-source PATH             書き出す音声を明示（省略時はassetから一意に解決）
--audio-bitrate KBPS            AAC bitrate（既定: 192）
--audio-from SEC / --audio-to SEC  書き出し範囲（秒）
--overwrite                     既存のM4Aを置換する
--export-video PATH             PreviewをMP4へ書き出す（Vulkanが必要）
--video-width PX / --video-height PX  出力解像度（既定: 1920x1080）
--video-fps FPS                 固定フレームレート（既定: 30）
--video-codec h264|h265|av1     映像codec（既定: h264）
--video-bitrate KBPS            映像bitrate（既定: 8000）
--video-from-frame N / --video-to-frame N  フレーム範囲（0始まり）
--no-audio                      MP4に音声streamを含めない
```

音声だけを書き出す場合は、ウィンドウやVulkanを起動せずに実行します。

```bash
./build/linux-debug/mikumikudesu \
  --asset music.wav \
  --export-m4a music.m4a

./build/linux-debug/mikumikudesu \
  --audio-source music.wav \
  --export-m4a chorus.m4a \
  --audio-bitrate 256 \
  --audio-from 35.0 --audio-to 78.5
```

出力は一時的な`.m4a.part`へ書き込み、成功時に完成ファイルへ原子的に置換します。Sceneの
背景・再生用`MediaFile`や画像の`OutputQueue`とは独立した`AudioExporter`を使います。

動画を書き出す場合はPreview rendererでSceneをタイムラインFPSのstepで評価し、指定解像度の
offscreen Vulkan targetをCPUへreadbackしてMP4へ渡します。`--video-fps`を変更してもモーションの
再生速度は変わりません。UIは動画に含めず、音声はassetから
一意に解決します（複数ある場合は`--audio-source PATH`で指定）。映像・音声のエンコード中は
`output.mp4.part`へ書き込み、完了時だけ`output.mp4`へ確定します。

```bash
./build/linux-debug/mikumikudesu \
  --asset miku.pmx --asset dance.vmd --asset camera.vmd --asset music.wav \
  --export-video dance.mp4 --video-width 1920 --video-height 1080 \
  --video-fps 30 --video-codec h264 --video-bitrate 8000

# 0〜300フレームを音声なしで書き出す
./build/linux-debug/mikumikudesu \
  --asset miku.pmx --asset dance.vmd --export-video preview.mp4 \
  --video-to-frame 300 --no-audio
```

動画書き出しはGPUを使うため、音声書き出しのような完全headless処理ではありません。SDL/Vulkan
のhidden windowとPreview deviceを起動します。Subayai/BDPTは現在のnative executor未実装のため、
動画書き出しではPreview rendererを使用してください。

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

`cmake/CompileHlsl.cmake`はDXCがあればVulkan 1.3向けSPIR-Vを生成し、Preview shaderは
glslc HLSL frontendへfallbackします。既存のbindless/ray query/ray tracing shaderを移す場合は
DXCが必須です。`SV_Barycentrics`はSPIR-V fragment barycentricへ写像する設計です。

`vcpkg.json`にはSDL3、Vulkan、DXC、DirectXTex、Bullet、Dear ImGui、ImGuizmo、cereal、
Jsonnetを宣言しています。Linux実行系の画像読込は現在stbと内蔵DDS decoderを使うため、
WICには依存しません。OIDNは任意で、HIP deviceが使えなければCPUへfallbackします。

## 構成

```text
src/
├── app/                     CLI、D&D、ImGui、main loop、project連携
├── core/                    Scene、PMX/VMD/VPD/VMdayo、animation、Bullet、media、effect、project、output
├── platform/                SDL3 window/event/audio
└── graphics/
    ├── device.hpp           API非依存device/resource契約
    └── vulkan/              Vulkan swapchain/pipeline/resource実装

deps/mikumikudayo.lock       取得するMikuMikuDayo Release ZIPの固定情報
MikuMikuDayo/                 初回セットアップ時に展開されるローカル依存（Git管理外）
scripts/fetch-mikumikudayo.py 依存ZIPの取得、検証、展開
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

## LICENCE

本プロジェクトのソースコードは、 [MIT License](LICENSE) です。

`MikuMikuDayo/` はGit管理せず、`deps/mikumikudayo.lock`で固定した
[MikuMikuDayo 1.30のRelease ZIP](https://github.com/pennennennennennenem/MikuMikuDayo/releases/download/MikuMikuDayo130/MikuMikuDayo130.zip)
をセットアップ時に取得します。
fetch時の必須ディレクトリ検証とCMake installは`deps/mikumikudayo-runtime.manifest`を共有します。
HLSL、renderer、postprocess、particle、sample、resとlicenceをテクスチャ・importを含めて
ディレクトリごと配布し、lockとmanifestも`share/mikumikudesu`に保存します。
インストール前に上記fetchスクリプトを実行してください。assetがない場合、installは失敗します。

MikuMikuDayo本体は upstream のMIT Licenseに従い、配布時はアーカイブ内のMikuMikuDayoおよびサードパーティのライセンス表示を保持してください。
