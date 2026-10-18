# 開発者向け情報

## ビルド、テスト、静的検査

`Makefile` は CMake Presets をラップします。ターゲット・引数は `make help`、preset は `make presets` で確認できます。

```sh
make setup
make test
make lint
make ci
./scripts/check-linux-amd.sh
```

`make test` は必要な上流配布物を取得してビルドし、CTest を実行します。`make ci` は警告をエラーにする構成です。AMD 向けスクリプトは Vulkan の確認、CTest、PMX / VMD / 画像 / `.dayo` の smoke test を実行します。詳細は[インストール](installation.md)を参照してください。

## CMake preset

| Preset | 用途 |
| --- | --- |
| `linux-debug` | Debug と Vulkan validation |
| `linux-release` | 汎用 Release |
| `linux-release-native` | ローカル CPU 向け最適化 |
| `linux-sanitize` | ASan / UBSan |
| `linux-system-only` | 依存ライブラリ取得を無効化、ImGui 無効 |
| `linux-clang-thinlto` | Clang ThinLTO |
| `linux-pgo-generate` / `linux-pgo-use` | PGO の収集と適用 |

## Linux native optimization profiles

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

## Linux release tuning

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

## 構成

```text
src/
├── app/                     CLI、D&D、ImGui、main loop、project連携
├── core/                    Scene、PMX/VMD/VPD/VMdayo、animation、Bullet、media、effect、project、output
├── platform/                SDL3 window/event/audio
└── graphics/
    ├── device.hpp           API非依存device/resource契約
    ├── native_renderer.*    effect要件判定とnative runtime lifecycle
    ├── subayai_*.{hpp,cpp}  deform/AS/material/environment/light契約
    ├── bdpt_*.{hpp,cpp}     accumulation/LUT/volume/runtime契約
    └── vulkan/              typed Vulkan resource/command/AS/pipeline/SBT実装

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

## 検証と関連資料

- [Preview 性能の測定条件](preview-performance.md)
- [MikuMikuDayo 1.30 の対応範囲](upstream-1.30.md)
- [Dayo 互換性の調査記録](dayo-audit/README.md)
- [Windows fixture 受け入れ条件](fixture_oracle.md)

過去の実測値と現行ビルドの性能は区別してください。変更の再現には、ドライバ、コンパイラ、モデル、preset、入力素材、キャッシュ状態を記録します。
