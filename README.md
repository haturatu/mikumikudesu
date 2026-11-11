# mikumikudesu

GNU/Linux 上で MMD のモデルやモーションを表示・編集するためのネイティブアプリケーションです。SDL3、Vulkan 1.3、ImGui を使い、PMX の表示、VMD の編集、カメラやライトの操作、音声・動画の出力を行います。

そもそも MMD のモデルを表示するだけなら単純に見えますが、実際にはボーンやモーフ、エフェクト、背景、描画順序などが絡みます。特に MikuMikuDayo の FX を Vulkan で動かそうとすると、単にシェーダーを変換するだけでは足りません。

なので、**通常のモデル・モーション編集と Preview 描画を使えるようにし、FX や RT を使う経路は互換性と GPU の条件を確認しながら拡張する**形にしています。

互換対象は MikuMikuDayo **1.30** です。必要な配布物はセットアップ時に取得し、アプリケーション自体のバージョン **0.1.0** と上流の互換対象バージョンは別々に管理しています。

**注意:** すべてのエフェクトや描画結果が本家と一致するわけではありません。通常の作業には Preview レンダラを使用してください。Subayai / BDPT は GPU と FX の条件を満たす場合にのみ有効になります。詳細は[互換性と制限事項](docs/upstream-1.30.md)を参照してください。

## 必要環境

まず Vulkan 1.3 に対応した環境が必要です。ただし Preview を使うだけならハードウェアレイトレーシングまで対応している必要はありません。

| 項目 | 要件 |
| --- | --- |
| OS | Linux x86_64 |
| ビルド | CMake 3.25 以降、Ninja、C++20 対応コンパイラ |
| GUI | SDL 3.2 以降 |
| 描画 | Vulkan loader、Vulkan 1.3 対応ドライバ、swapchain 対応 GPU |
| シェーダー | `glslc` または DXC |
| セットアップ | Git、Python 3 |

AMD GPU では Mesa RADV を推奨します。Preview の利用にハードウェアレイトレーシングは不要です。Subayai / BDPT の検証には、要求される Vulkan RT feature を備えた GPU が必要です。BDPT を使用する構成では RAM 16 GiB、VRAM 8 GiB 以上を推奨します。

### Arch Linux / Artix Linux

Arch Linux / Artix Linux の AMD GPU なら、例えば以下のパッケージで始められます。別の GPU なら Vulkan ドライバだけ対応するものに置き換えてください。

AMD GPU でのパッケージ導入例:

```sh
sudo pacman -S --needed \
  git python base-devel cmake ninja sdl3 ffmpeg bullet nlohmann-json \
  vulkan-headers vulkan-icd-loader vulkan-radeon vulkan-tools \
  vulkan-validation-layers shaderc
```

AMD 以外の GPU では、使用する GPU に対応する Vulkan ドライバを導入してください。通常の CMake preset は argparse、stb、Dear ImGui、Jsonnet などの一部の依存関係を必要に応じて取得します。外部取得を禁止する `linux-system-only` preset では、開発用パッケージを別途揃える必要があります。

ドライバの認識状況は `vulkaninfo --summary` で確認できます。

## ビルド

初回だけ注意したいのが、通常の CMake ビルドとは別に MikuMikuDayo の配布物を取得する必要があることです。先にセットアップを行ってからビルドします。

### ソースの取得と初期設定

submodule と、互換性の基準にしている上流 1.30 のアセットを用意します。

```sh
git clone --recurse-submodules https://github.com/haturatu/mikumikudesu.git
cd mikumikudesu
make setup
```

`make setup` は `external/libmmd` を初期化し、`deps/mikumikudayo.lock` に固定された MikuMikuDayo 1.30 の Release ZIP を取得・検証して `MikuMikuDayo/` に展開します。ZIP は `.cache/mikumikudayo/` から再利用できます。すでに clone した作業ツリーでは、`git submodule update --init --recursive` で submodule を更新できます。 現在のlibmmd固定バージョンは [v0.3.0](https://github.com/haturatu/libmmd/releases/tag/v0.3.0) です。

**注記:** `make` および `make build` は MikuMikuDayo の配布物を取得しません。初回は先に `make setup` を実行してください。

### システムにインストールした libmmd を使う

PMX / VMD の処理には libmmd を使っています。毎回 submodule をビルドするのではなく、システムに入っている libmmd を利用することもできます。

Arch Linux と Artix Linux では、AUR の `libmmd-cpp` パッケージから libmmd をインストールできます。

```sh
yay -S libmmd-cpp
cmake -S . -B build/system -G Ninja -DCMAKE_BUILD_TYPE=Release -DLIBMMD_PROVIDER=SYSTEM
cmake --build build/system --parallel
```

ほかのビルド依存関係も必要です。`LIBMMD_PROVIDER` は既定で `AUTO` です。互換性のある libmmd 0.3.2 以降の 0.3 系を検出するとシステム版を使い、見つからなければ組み込み版を使います。`SYSTEM` はシステム版を必須にし、`BUNDLED` は従来の組み込み版を使います。システム版を選ぶと libmmd の submodule 初期化やソース取得は不要です。CMake を直接実行する場合にも、アプリケーション固有のアセットや依存関係を準備してください。

### Debug ビルド

開発中や動作確認だけなら、まず Debug 版をビルドするのが簡単です。生成したバイナリを直接起動でき、毎回 Makefile を通す必要はありません。

```sh
make
```

`make` は `linux-debug` preset で CMake の設定とビルドを実行します。生成される実行ファイルは `build/linux-debug/mikumikudesu` です。

ビルド済みの実行ファイルは、Makefile を経由せずに起動できます。

```sh
./build/linux-debug/mikumikudesu
./build/linux-debug/mikumikudesu --asset /path/to/model.pmx
```

`/path/to/model.pmx` は実際のモデルファイルのパスに置き換えてください。

### Release ビルドとテスト

配布用のビルドと、実装変更を検証するテストは分けています。Release バイナリを作るだけの場合に、すべてのテストを混ぜて実行する必要はありません。

```sh
make release
./build/linux-release/mikumikudesu --asset /path/to/model.pmx

make test
```

`make release` は `linux-release` preset、`make test` は `setup` とビルドの後に CTest を実行します。`make help` でほかのターゲットを確認できます。

CMake を直接使う場合は、次のようにビルドできます。

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug --output-on-failure
```

`linux-system-only` は依存関係の自動取得と ImGui を無効化します。通常の GUI ビルドとは機能が異なります。

```sh
cmake --preset linux-system-only
cmake --build --preset linux-system-only
ctest --preset linux-system-only --output-on-failure
```

## モデルとモーションの読み込み

まず PMX と VMD を開き、モデルとモーションが正しく読み込まれているか確認します。MMD の素材は配布条件がそれぞれ異なるので、テストや配布で扱う場合も注意が必要です。

`--asset` には、モデル、モーション、画像、音声などの入力ファイルを指定します。同じオプションを複数回指定すると、複数のファイルを読み込めます。

```sh
./build/linux-debug/mikumikudesu \
  --asset /path/to/model.pmx \
  --asset /path/to/dance.vmd \
  --asset /path/to/background.png
```

GUI へのドラッグ＆ドロップでもファイルを読み込めます。

同じモデルに VMD を追加で読み込むと、既存のキーを残して新しいキーを取り込みます。同じボーン名またはモーフ名の同じフレームにキーが重なる場合は、後から読み込んだ値が優先されます。ダンス用の VMD に表情用の VMD を追加する場合にも使用できます。

### サンプルモデル

動作確認用のモデルは、配布元から取得して `assets/models/` 以下に配置できます。モデル本体の再配布条件は、各配布物の README に従ってください。

| モデル | ローカルでのパス | 配布元 |
| --- | --- | --- |
| Tda 式初音ミク デフォ服 ver. | `assets/models/miku/model/miku.pmx` | [BowlRoll #16344](https://bowlroll.net/file/16344) |
| Tda 式重音テト TypeS | `assets/models/teto/model/teto.pmx` | [BowlRoll #11308](https://bowlroll.net/file/11308) |

上記はローカルに取得・展開した場合のパスであり、リポジトリにモデルファイルが同梱されていることを意味しません。重音テトを使った作品の公開・改変時には[公式ガイドライン](https://kasaneteto.jp/guidelines/)も確認してください。

## GUI の操作

普段の操作は GUI から行います。モデルの表示、カメラの操作、キーフレームの編集を分けているので、必要な作業領域を選んでください。

### Viewport

Viewport はモデルの表示を確認する場所です。見た目の確認と編集対象の選択が混ざらないよう、操作モードを分けています。

| 操作 | 動作 |
| --- | --- |
| 左ドラッグ | カメラの回転 |
| 右ドラッグ | カメラの平行移動 |
| マウスホイール | ズーム |
| `Reset view` または右クリックメニュー | 視点のリセット |

カメラ VMD の読み込み時に `Follow VMD camera` が有効だと、手動のカメラ操作はロックされます。操作するには無効にしてください。無効化すると現在の視点と画角を保ったまま操作できます。`Reset view` は追従によるロックも解除します。平行移動とズームの操作量はモデルの正規化スケールを考慮します。

### カメラとライトの編集

カメラやライトはモデルのボーンとは別のトラックです。再生結果を確認しながら、どのトラックを編集しているかを意識してください。

Viewport 上部の `Camera` または `Light` を選ぶと、Inspector に対応する編集欄が表示されます。Inspector の `Selection` / `Camera` / `Light` から切り替えることもできます。値の変更はプレビューへ即座に反映されます。

変更を VMD に書き出すには、次の順番で操作します。

1. `Register camera key` または `Register light key` で、現在のフレームにキーを登録します。
2. Inspector の `VMD file` に保存先のパスを指定します。
3. `Export camera VMD` または `Export light VMD` を選択します。

キー登録前のプレビュー変更は、エクスポートされません。読み込む場合は `VMD file` を指定し、`Import camera keys` または `Import light keys` を選択します。読み込みは対象のトラックを置き換え、ほかのトラックは保持します。キー登録は `Ctrl+Z` で取り消せます。

### キーフレームの編集

モーションが長くなるとキーの数も増えるので、一覧から探すだけでは編集しにくくなります。タイムライン上で直接選択・移動できるようにしています。

上部の `Animation` に切り替えると、タイムラインと Inspector を並べて編集できます。検索は、モデルが持つ日本語名と英語名に対応します。トラックの種類でボーン・モーフ・カメラなどを絞り込めます。

| 操作 | 動作 |
| --- | --- |
| キーをクリック | 選択する（選択キーは金色） |
| キーをドラッグ | フレーム位置を移動する |
| `Ctrl` + クリック | 選択の追加・解除 |
| `Shift` + クリック | 同じトラックの範囲選択 |
| 空白からドラッグ | 矩形選択 |
| `Ctrl+A` | 検索・絞り込みに一致するキーを全選択 |
| `Esc` | キーのドラッグをキャンセル |
| 左右キー / `-1f` / `+1f` | 1 フレーム移動 |
| `Shift` + 左右キー | 10 フレーム移動 |
| `Ctrl+Z` / `Ctrl+Y` | 取り消し・やり直し |

選択したキーの値は Inspector で変更し、`Apply key` で確定します。複数選択時は `Offset (frames)` と `Move selected keys` でまとめて移動できます。`Clear key selection` でキーの選択を解除できます。

目盛りのクリック・ドラッグで再生位置を指定できます。`Ctrl` + ホイールでズーム、中ボタンドラッグまたは `Shift` + ホイールで横スクロールし、`Fit` で表示範囲を戻します。`Prev key` / `Next key` でもキー間を移動できます。実際の操作と検証内容は[キーフレームエディタの検証記録](docs/ui-ux/keyframe-editor.md)を参照してください。

### 音声と背景動画

MP4 をドラッグ＆ドロップすると、初期状態では音声を読み込み、波形を表示してタイムラインに同期します。映像は `Media` メニューまたはタイムラインの `Media` にある `Show MP4 video` から切り替えます。非表示にすると映像のデコードと GPU 転送を停止しますが、読み込んだファイルと再生位置は保持します。

背景動画を別に指定するには、Scene の Inspector の `Video file` にパスを入力し、`Apply video file` を選択します。`Volume` と `Audio offset` で音量と音声の時間ずれを調整できます。再生速度の変更は音声にも適用され、ピッチも変わります。音声を含まない MP4 は、音声読み込みエラーになります。

### 表示言語

上部の `Language / 言語 / 语言 / Язык` で英語、日本語、簡体字中国語、ロシア語を選択できます。初回は OS の優先言語を使用し、未対応なら英語を使用します。

選択は通常 `~/.local/share/haturatu/mikumikudesu/language.txt` に保存されます（実際の場所は SDL のユーザーデータディレクトリに依存します）。モデル名、ボーン名、モーフ名、ファイルパスは元のデータの表記を維持します。

## レンダラと画質

すべてを最高画質で描画するより、普段は軽い Preview で編集し、必要なときだけ高画質の経路を使う方が扱いやすいと思います。

### Preview

通常のモデル・モーションの確認では Preview を使ってください。ハードウェア RT がなくても動作することを想定しています。

Preview は RT 機能を必要としない描画経路です。シャドウマップ、GGX 直接反射、IBL、SSAO、RGBA16F 合成、ACES トーンマッピングを使用します。2:1 の equirectangular HDRI を Scene の Inspector から照明用に指定できます。指定しない場合は、条件に合う背景画像を照明に使用します。

モデルの Inspector では、材質ごとに `PMX`、`Skin`、`Hair`、`Cloth`、`Metal`、`Plastic`、`Glass` を選択できます。選択した内容はプロジェクトに保存されます。

Viewport の `Effects` または Render ワークスペースの Inspector から、AO（SSAO/GTAO）、肌SSS、髪のハイライト、布の光沢、Clearcoat、PCSS、Bloom、FXAA、被写界深度を切り替えられます。プリセットに対する個別上書きとリセット、ACES／AgX／Neutral、露出・ホワイトバランスに対応し、設定は `.dayo` に保存されます。材質の効果は Skin／Hair／Cloth／Plastic／Glass プリセットに対応します。方式の詳細と制限は[描画エフェクト](docs/preview/render-effects.md)を参照してください。

画質 profile は `--preview-quality auto`、`apu`、`high` から選択できます。既定値の `auto` は GPU の種類とメモリ使用状況を考慮します。`--present` では Present mode、`--async-compute` では実験的な別キューでの変形処理を指定できます。これらの設定と Vega 8 での計測結果は[Preview の性能](docs/preview-performance.md)を参照してください。

### Subayai / BDPT

この二つは Preview の単なる画質設定ではありません。対応 GPU、読み込む FX の要求機能、ネイティブ runtime の初期化がそろった場合だけ利用できます。

Subayai / BDPT では、FX graph が必要とする機能、GPU の Vulkan capability、native runtime の初期化結果を起動時に確認します。いずれかが不足すると理由を記録し、Preview に切り替わります。native renderer の接続と、すべての GPU・FX で本家と同じ結果が得られることは別です。

主な RT 関連の確認対象は次のとおりです。

- `VK_KHR_acceleration_structure`
- `VK_KHR_ray_tracing_pipeline`
- `VK_KHR_ray_query`
- `VK_KHR_fragment_shader_barycentric`
- Buffer device address / descriptor indexing

```sh
./build/linux-debug/mikumikudesu --probe --hidden
./build/linux-debug/mikumikudesu --renderer subayai --asset /path/to/model.pmx
```

`--probe` で GPU の機能情報を確認できます。互換性の詳細と未検証事項は[1.30 互換性](docs/upstream-1.30.md)に記載しています。

## コマンドライン

GUI で確認するだけでなく、決まった条件で画像・動画を書き出したり、自動テストから起動したりする場合は CLI が便利です。

オプション一覧は次のとおりです。`PATH`、`N`、`KBPS` などは指定する値を表し、そのまま入力する文字列ではありません。

### 読み込み・描画

| オプション | 説明 |
| --- | --- |
| `--asset PATH` / `--model PATH` | 入力ファイル。複数指定可能 |
| `--save-project PATH` | プロジェクトを `.dayo` に保存 |
| `--renderer RENDERER` | `preview`、`subayai`、`bdpt`（既定: `preview`） |
| `--preview-quality PROFILE` | `auto`、`apu`、`high`（既定: `auto`） |
| `--present MODE` | `fifo`、`fifo-relaxed`、`mailbox`、`immediate`（既定: `fifo`） |
| `--async-compute` | Preview の変形処理を別キューへ送る実験的な設定 |
| `--probe` | GPU の機能情報を出力して終了 |
| `--hidden` | 非表示ウィンドウを使用 |
| `--frames N` | N フレーム描画後に終了 |
| `--no-validation` | Vulkan validation を無効化 |

### 音声・動画の書き出し

| オプション | 説明 |
| --- | --- |
| `--export-m4a PATH` | 音声を M4A/AAC に書き出す |
| `--export-video PATH` | 映像を MP4 に書き出す |
| `--audio-source PATH` | 音声ソースを明示 |
| `--audio-bitrate KBPS` | AAC ビットレート（既定: 192） |
| `--audio-from SEC` / `--audio-to SEC` | 音声の出力範囲。M4A のみ |
| `--video-width PX` / `--video-height PX` | 動画解像度（既定: 1920 × 1080） |
| `--video-fps FPS` | 動画 FPS（既定: 30） |
| `--video-codec CODEC` | `h264`、`h265`、`hevc`、`av1`（既定: `h264`） |
| `--video-bitrate KBPS` | 映像ビットレート（既定: 8000） |
| `--video-hardware` | ハードウェアエンコードを優先 |
| `--video-from-frame N` / `--video-to-frame N` | 動画の出力フレーム範囲（0 始まり） |
| `--no-audio` | MP4 に音声を含めない |
| `--overwrite` | 既存の出力ファイルを置き換える |

`--export-m4a` と `--export-video` は同時に使用できません。また、動画専用のオプションは `--export-video` と組み合わせ、`--audio-from` / `--audio-to` は `--export-m4a` と組み合わせてください。

### M4A への音声書き出し

音声書き出しではウィンドウや Vulkan を起動しません。

```sh
./build/linux-debug/mikumikudesu \
  --audio-source music.wav \
  --export-m4a chorus.m4a \
  --audio-bitrate 256 \
  --audio-from 35 --audio-to 78.5
```

出力は一時的な `.m4a.part` ファイルに書き込み、成功した場合に完成ファイルへ確定します。

### MP4 への動画書き出し

動画書き出しは Preview と Vulkan の非表示ウィンドウを使用します。GUI は映像に含まれません。完全にディスプレイ環境に依存しない処理ではありません。

```sh
./build/linux-debug/mikumikudesu \
  --asset miku.pmx --asset dance.vmd --asset camera.vmd --asset music.wav \
  --export-video dance.mp4 \
  --video-width 1920 --video-height 1080 \
  --video-fps 30 --video-codec h264 --video-bitrate 8000
```

`--video-fps` を変更してもモーションの再生速度は変わりません。複数の音声ソースがある場合は `--audio-source` で指定します。出力中は `.mp4.part` ファイルを使用します。

## 開発と検証

FX の変換や GPU リソースの処理は、コンパイルできたことと正しく表示されることが別です。そのため Mock/CPU テストと GPU 実機での確認を分けています。

### 診断とテスト

```sh
make help
make presets
make test
./scripts/check-linux-amd.sh
```

AMD 向けの診断スクリプトは `vulkaninfo` による確認、CTest、PMX/VMD、画像、`.dayo` の保存・再読み込み、FFmpeg の smoke test を実行します。実行前に submodule を初期化してください。

### ビルドの最適化

通常の Release はローカル CPU 専用の ISA 最適化を無効にしています。対象マシン専用にビルドする場合は `linux-release-native` を使用できます。

```sh
cmake --preset linux-release-native
cmake --build --preset linux-release-native
```

Clang ThinLTO と PGO も任意で有効にできます。

```sh
cmake --preset linux-clang-thinlto
cmake --build --preset linux-clang-thinlto
```

PGO では、profile 収集用にビルドして実際の作業に近い入力を与えてから、収集したデータを適用します。

```sh
cmake --preset linux-pgo-generate
cmake --build --preset linux-pgo-generate
./build/linux-pgo-generate/mikumikudesu --asset model.pmx
cmake --build build/linux-pgo-generate --target dayo_pgo_merge
cmake --preset linux-pgo-use
cmake --build --preset linux-pgo-use
```

LLVM BOLT は CPU の profile を取得した後に有効化する追加の最適化です。同じビルド済みバイナリと link 条件で収集・適用してください。初回は relocation を出力し、profile を取得してから同じビルドツリーで BOLT を有効にします。

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

BOLT の出力は `mikumikudesu.bolt` です。有効時の install はこの最適化済みバイナリを `mikumikudesu` として配置します。詳細な GPU 側の最適化・測定条件は[Preview の性能](docs/preview-performance.md)を参照してください。

現行 Release の ELF・SPIR-V・GPU ISA と実行時プロファイルを調べた[バイナリ性能調査](docs/performance/binary-audit.md)には、背景動画、トーンマップ、頂点配列確保の最適化候補と再現手順を記録しています。

### ソースコードの構成

| パス | 内容 |
| --- | --- |
| `src/app/` | CLI、GUI、イベント処理、起動 |
| `src/core/` | シーン、アニメーション、物理、メディア、プロジェクト、出力 |
| `src/platform/` | SDL3 ウィンドウ・イベント・音声 |
| `src/graphics/` | レンダラと Vulkan リソース |
| `external/libmmd/` | PMX、PMD、VMD、VPD などを扱うライブラリ |
| `deps/` | 上流配布物の固定情報 |
| `scripts/` | セットアップ・診断スクリプト |

## 動作確認と制限事項

実装されている機能が、すべてのモデル・ドライバ・エフェクトで同じ結果になるとは限りません。特に RT と上流 FX の互換性は、環境をそろえて確認する必要があります。

2026-08-24 に AMD Ryzen 5 PRO 3500U / Radeon Vega 8 / Mesa RADV 26.1.7 で、Preview の描画、VMD・PMX と Bullet の処理、画像読込、動画・音声、`.dayo` の保存・再読込を確認しています。この環境は Vulkan RT feature を備えていないため、Subayai / BDPT の GPU 実行結果は検証していません。

この結果は記載した環境・日時のものです。現在の実装状況や GPU 機能の判断には `--probe` と[互換性資料](docs/upstream-1.30.md)を参照してください。

## ライセンス

本プロジェクトのソースコードは [MIT License](LICENSE) です。

MikuMikuDayo 1.30 の配布物は[公式リリース](https://github.com/pennennennennennenem/MikuMikuDayo/releases/tag/MikuMikuDayo130)から取得します。配布物の取得には `scripts/fetch-mikumikudayo.py` を使用し、`deps/mikumikudayo-runtime.manifest` の内容を install と共有します。再配布する場合は、上流配布物とそのサードパーティライブラリのライセンス表示を保持してください。

モデル、モーション、テクスチャ、音声などの素材には、本プロジェクトのライセンスとは別に、それぞれの利用条件が適用されます。
