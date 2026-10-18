# インストール

## 必要環境

- Linux x86_64
- CMake 3.25以上、Ninja、C++20コンパイラ
- argparse 3.2（通常presetはCMakeが取得、system-onlyはCONFIG packageが必要）
- SDL 3.2以上
- Vulkan loaderとVulkan 1.3対応ドライバ
- glslc、またはDXC
- Preview: Vulkan swapchain対応GPU
- RT実装の開発・検証: RDNA2/RX 6000以降など、起動時に表示するRT featureを満たすGPU

AMDではMesa RADVを推奨します。BDPTを将来有効化する構成ではRAM 16GB、VRAM 8GB以上を
推奨します。

## Arch / Artix Linux

```bash
sudo pacman -S --needed \
  base-devel cmake ninja sdl3 ffmpeg bullet nlohmann-json \
  vulkan-headers vulkan-icd-loader vulkan-radeon vulkan-tools \
  vulkan-validation-layers shaderc
```

不足するVulkan-Headers、stb、Dear ImGui、Bullet、Jsonnet等は、通常presetでは固定versionを
CMakeが取得します。argparseも同様ですが、取得を禁止する場合はargparseを含む必要なdevelopment
packageを先に導入してください。

上記はパッケージの導入例です。`git` と `python` も必要です。GPU に応じて正しい Vulkan ICD を使用してください。ほかのディストリビューションでは相当する development package を導入します。

## ソースの取得

```sh
git clone --recurse-submodules https://github.com/haturatu/mikumikudesu.git
cd mikumikudesu
make setup
```

既存の clone を使う場合は `git submodule update --init --recursive` で `external/libmmd` を初期化します。`make setup` は submodule を初期化し、`deps/mikumikudayo.lock` に固定した Release ZIP をダウンロード・検証して展開します。展開先は Git 管理外の `MikuMikuDayo/` です。

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

依存関係としてMikuMikuDayoが必要ですが、`make`/`make build`はMikuMikuDayoを取得しません。MikuMikuDayoは`git`管理されていないのでリリースから取得するようになっています。`make setup`、`make test`、`make install`、
`make package`でだけ、`deps/mikumikudayo.lock`に固定したGitHub Release ZIPを取得または
`.cache/mikumikudayo/`から再利用します。

`Makefile` の全ターゲットと変数は`make help`で確認できます。CMakeを直接実行する場合は以下です。

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
python3 scripts/fetch-mikumikudayo.py
ctest --preset linux-debug --output-on-failure

./build/linux-debug/mikumikudesu
./build/linux-debug/mikumikudesu --asset model.pmx --asset motion.vmd
```

`make` / `make build` 自体は MikuMikuDayo の取得を行いません。`make test`、`make install`、`make package` は `setup` を実行します。CMake を直接使う場合は、上記のように取得スクリプトを明示的に実行してください。

## インストールと system-only ビルド

```sh
make install PREFIX="$HOME/.local/mikumikudesu"
```

`install` は upstream 配布物がない場合に失敗します。システム上の依存パッケージだけを使い、依存関係の取得を禁止する preset は次のとおりです。

```bash
cmake --preset linux-system-only
cmake --build --preset linux-system-only
ctest --preset linux-system-only --output-on-failure
```

**注意:** `linux-system-only` は `DAYO_ENABLE_IMGUI=OFF` に設定され、通常の GUI 向けビルドとは機能が異なります。必要な依存ライブラリが CONFIG package として見つかるように設定してください。

## 関連項目

- [CLI リファレンス](cli.md)
- [トラブルシューティング](troubleshooting.md)
- [開発者向け情報](development.md)
