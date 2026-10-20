# #311: native / ThinLTO / PGO の比較と採用判断

標準の `linux-release` を維持する。今回の実機計測ではnative・ThinLTO・PGOによる再生時間の改善を確認できなかった。
Clang Release / ThinLTOは実行ファイルの小型化を目的とするローカルの選択肢として残す。PGOとnativeは実験用の明示指定とし、配布用には採用しない。
とくにnativeは固定フレームの出力にも差分があり、速くなるという前提で有効化しない。

## 実装

- `mmd_core` / `mmd_animation` / `mmd_physics` までnative・ThinLTO・PGO・sanitizer設定を適用。システムのビルド済みBulletは対象外。
- 静的ライブラリの利用側にも必要なLTO/PGO/sanitizerリンク設定を伝播する。既定のRelease設定は変更しない。
- Clang Release + LLDの比較用presetを追加し、Clang各構成のリンカを揃える。
- Clang PGOの保存先はコンパイル時にも埋め込む。リンク時だけの指定では保存先が反映されない問題を修正。
- プロファイルマージの引数に混入していた引用符を除去。別コンパイル/リンクと、空白を含むディレクトリで生成・マージを実検証。
- `DAYO_BUILD_BENCHMARKS=ON` でモデルCPUロード計測用の `dayo_model_load_bench` を明示ビルドできる。通常ビルドには追加しない。

## 条件と制限

2026-10-20〜21、Ryzen 5 PRO 3500U（4 cores / 8 threads）、Vega 8 / RADV RAVEN。
GCC 16.2.1、Clang/LLD/llvm-profdata 23.1.3、CMake 4.4.4-dirty。
同一の製品ソースとlibmmd `14e1528` を使用。最初の構成は `c4d6d50`、PGO設定と計測ツールを修正した再開時は `ea9fb0a`。
`src`・`shaders`・libmmdの差分がないことを確認して再利用した。PGOは修正後に新規ビルドした。

構成ごとに独立した空のビルドディレクトリ、同じ取得済み依存ソース、`-j3`、コンパイラキャッシュなし。
ビルド時間は `mikumikudesu` ターゲットの初回ビルドで、configure・依存ソースのダウンロード・追加ロード計測ツールのビルド・PGO学習時間を含まない。
各構成のPMX読込・animation・physics・imageのcompile_commandsで実際の最適化フラグを検査した。

Teto + `telephone_merged.vmd`、1/3/5モデル、各300フレーム×3回の中央値。60フレームを事前実行。
Preview / hidden / immediate / APU。ウィンドウ1280×720、ログで確認した描画Viewportは732×457。1080pの計測ではない。
各構成のビルドと時間計測を順番に実施し、同時に別のビルドやテストは走らせていない。
ただしCPU/GPUクロック・温度は固定しておらず、APUの電力共有、連続ビルド後の温度、実時間で進むVMDと適応物理の影響がある。
構成の実行順も固定なので、表の差をコンパイラ最適化だけの因果効果として扱えない。速度向上を裏付ける結果ではない。

## 再生時間

単位ms。各列は3回の中央値。convert/uploadはログの小数2桁の精度であり、0.00は処理時間ゼロを意味しない。
p95は各300フレーム実行のフレーム時間p95の中央値。CPUとGPUは非同期であり、合算しない。

| 構成 | モデル数 | animation | convert | upload | render | gpu | p95 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| gcc-release | 1 | 1.16 | 0.00 | 0.00 | 1.59 | 3.07 | 4.27 |
| gcc-release | 3 | 3.02 | 0.00 | 0.01 | 1.73 | 5.05 | 6.63 |
| gcc-release | 5 | 5.18 | 0.00 | 0.01 | 1.64 | 6.97 | 10.20 |
| gcc-native | 1 | 1.26 | 0.00 | 0.01 | 1.78 | 3.28 | 4.34 |
| gcc-native | 3 | 3.36 | 0.00 | 0.01 | 2.04 | 5.79 | 7.23 |
| gcc-native | 5 | 7.70 | 0.00 | 0.01 | 1.17 | 8.52 | 12.50 |
| clang-release | 1 | 1.10 | 0.00 | 0.00 | 1.70 | 3.09 | 3.89 |
| clang-release | 3 | 3.14 | 0.00 | 0.01 | 1.88 | 5.39 | 6.03 |
| clang-release | 5 | 6.39 | 0.00 | 0.01 | 1.55 | 7.97 | 11.73 |
| clang-thinlto | 1 | 1.20 | 0.00 | 0.00 | 1.73 | 3.18 | 4.73 |
| clang-thinlto | 3 | 3.44 | 0.00 | 0.01 | 1.94 | 5.63 | 7.28 |
| clang-thinlto | 5 | 7.66 | 0.00 | 0.01 | 0.97 | 8.41 | 12.38 |
| clang-pgo-use | 1 | 1.38 | 0.00 | 0.00 | 1.95 | 3.62 | 4.50 |
| clang-pgo-use | 3 | 3.86 | 0.00 | 0.01 | 2.03 | 6.13 | 7.84 |
| clang-pgo-use | 5 | 7.43 | 0.00 | 0.01 | 1.34 | 8.42 | 12.80 |

PGO generateは計測候補ではなく、学習用の計装ビルド。1/3モデル各600フレーム（各warmup 60）、
固定2フレーム出力、空シーン起動、モデル/VMDロードを実行し、14個の実プロファイルをマージしてuseを構築した。
GPU各パスの値と個別runは [issue-311-results.json](issue-311-results.json) に収録。

## ビルド・起動・ロード

実行ファイルサイズはstrip前。起動はプロセス開始から1フレーム描画・終了までのwall時間（純粋なUI ready時間ではない）。
「起動+assets」にはモデル/VMD読み込み、GPU準備、1フレーム描画と終了も含む。1回warmup後に3回測定した中央値。
CPUモデルロードは `Scene::addModel()` のPMX解析・テクスチャ読込・animator/物理初期化を含み、GPU転送を含まない。
VMDは `loadVmd()` + `importMotion()`。同一プロセスで1回warmup後に3回の中央値を測定し、通常再生とは区別した。

| 構成 | 初回ビルド s | 実行ファイル MB | 起動 ms | 起動+assets ms | CPUモデル ms | VMD ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| gcc-release | 260.8 | 26.57 | 336.0 | 583.5 | 44.48 | 95.71 |
| gcc-native | 294.2 | 26.52 | 356.1 | 607.0 | 41.70 | 94.57 |
| clang-release | 233.5 | 16.23 | 353.9 | 628.9 | 41.56 | 96.12 |
| clang-thinlto | 221.4 | 15.52 | 339.6 | 579.9 | 40.94 | 100.09 |
| clang-pgo-generate | 233.2 | 17.49 | 352.4 | 656.2 | 60.43 | 102.18 |
| clang-pgo-use | 242.1 | 15.90 | 374.9 | 615.5 | 45.84 | 94.92 |

ビルド中のcmakeプロセスツリーを `/proc` から1秒ごとに収集したRSS合計の観測最大値:

| 構成 | 観測最大RSS MiB | サンプル数 |
| --- | ---: | ---: |
| clang-release | 1001.3 | 132 |
| clang-thinlto | 1012.6 | 206 |
| clang-pgo-generate | 1020.5 | 218 |
| clang-pgo-use | 1018.1 | 225 |

GCC構成は未収集、Clang Releaseはビルド途中から収集。共有ページはプロセス間で重複計上され、1秒未満のピークは欠落する。
したがって正確な物理メモリ最大値や、LTOの追加メモリ量を確定する数値ではない。
今回のThinLTOビルドは221.4秒、Clang Releaseは233.5秒だったが、各構成1回・温度/キャッシュ非固定なのでビルドが常に速くなるとは判断しない。
PGOはgenerate 233.2秒 + 学習/マージ + use 242.1秒を必要とし、単一ビルドの時間だけでは採否を判断できない。

## 画像の一致と採用判断

フレーム30/31を320×320で動画出力し、同じffmpegでRGBへデコードして全614,400チャンネルを比較。
Clang Release・ThinLTO・PGO generate/useはGCC Releaseと完全一致した。SHA-256:
`16d68dd6e2417ecd6502719f0c84b80410409142778ca589adfa65d572f7e705`。
これは固定2フレームの比較であり、全アニメーションの一致の証明ではない。

nativeは36,949チャンネルに差分があり、平均絶対誤差0.2233/255、最大244/255。
差分の原因をFMAなどに特定してはいない。この差分と再生改善を確認できなかったことから、既定化しない。

| 用途 | 判断 |
| --- | --- |
| 配布・通常開発 | `linux-release` を維持。CPU固有命令を無条件に有効化しない。 |
| ローカルで実行ファイルを小さくしたい | `linux-clang-release`（16.23 MB）または `linux-clang-thinlto`（15.52 MB）を明示選択。再生高速化は未確認。 |
| PGO | 学習/マージ/利用まで修正・検証。今回のワークロードでは採用根拠となる速度改善なし。実験用として維持。 |
| native | ローカル実験用。生成したCPUへの依存と画像差分があるため、配布用には採用しない。 |
| nativeレンダラ | このVega 8 / DXC未設定環境では実GPUのSubayai/BDPT比較は未実施。 |

先行するCPU変更の命令数と呼出し回数は [optimization-callgrind.md](optimization-callgrind.md)。
inclusive値を重複加算せず、命令数からFPS改善率へ換算しない。

## 検証

- 通常Releaseの有効32テストが全件成功（通常/High/非同期Preview、1080p DoF輪郭、native CPU/mock、動画出力を含む）。既存のregression_testは無効。
- ThinLTO・PGO generate・PGO useそれぞれでcore/image_blocksテストを新規ビルドし、リンクと実行に成功。
- 上記の各構成でPMX/VMDロードと実GPU Preview再生・固定フレーム出力を実行。実GPUのnative RTは対象外。
- GCC/Clang Debug、GCC Release、system-only、CodeQL、format/clang-tidyがGitHub Actionsで成功。

## 再現手順

通常Releaseと任意のClang構成。必要なClang/LLDをインストールしてから実行する。

```sh
cmake --preset linux-release
cmake --build --preset linux-release -j3
ctest --preset linux-release --output-on-failure

cmake --preset linux-clang-release
cmake --build --preset linux-clang-release -j3

cmake --preset linux-clang-thinlto
cmake --build --preset linux-clang-thinlto -j3
```

全比較を再実行する。`--output` は未使用ディレクトリ、`--deps` は同じ依存ソースを使った既存ビルドの `_deps`。
ディスプレイ/Vulkan/ffmpegと同じモデル・モーションが必要。出力ディレクトリには構成別ログ・全計測JSON・動画・PGOプロファイルを保存する。

```sh
python docs/performance/compare_builds.py \
  --output "$HOME/.cache/mikumikudesu-build-comparison" \
  --deps build/linux-release/_deps \
  --model assets/models/teto/model/teto.pmx \
  --motion telephone_merged.vmd --jobs 3
```

PGOを手動で使う場合もgenerateとuseは同じClang/llvm-profdataバージョン・製品ソースを使い、古いプロファイルを混ぜない。
generateをビルドして、モデルロードと代表的なPreview再生・出力を実行した後にmergeする。

```sh
cmake --preset linux-pgo-generate
cmake --build --preset linux-pgo-generate -j3
python scripts/benchmark-preview.py \
  --binary build/linux-pgo-generate/mikumikudesu \
  --model assets/models/teto/model/teto.pmx \
  --motion telephone_merged.vmd --models 1 3 --frames 600 --runs 1 \
  --output "$HOME/.cache/mikumikudesu-pgo-training"
cmake --build build/linux-pgo-generate --target dayo_pgo_merge
cmake --preset linux-pgo-use
cmake --build --preset linux-pgo-use -j3
```
