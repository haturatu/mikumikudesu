# レンダリング／Preview のバイナリ性能調査（2026-10-23）

現行 Release の x86-64 ELF、生成 SPIR-V、RADV が生成した GFX9 ISA と実行時の呼出しを調査した。
次に着手する候補は **背景動画の連続デコード、トーンマップの画素不変計算の移動、未使用頂点配列の確保抑制**。
CPU アニメーション区間は Bullet が支配しており、アプリ側の小さな確保削減だけで大幅な FPS 改善を見込む根拠はない。

本 PR は調査・再現ツール・実測記録を追加する。製品の描画、物理、ビルド設定は変更しない。
下記の時間は現状の観測値であり、提案した変更の改善率ではない。
機械可読な数値と入力ハッシュは [binary-audit-results.json](binary-audit-results.json) に保存した。

## 優先度と根拠

| 順位 | 対象 | 確認できたこと | 次の変更と検証 |
| --- | --- | --- | --- |
| 1 | 背景動画 | 連続60フレームでも60回 seek/flush。GOP 60 では1,830パケットを再デコード | 順送りではデコーダを保持し、逆送り・ジャンプのみ seek。B-frame/VFR/EOF/ループ/音声共存を検証 |
| 2 | トーンマップ | GFX9 の静的コード22,576 bytes、3,524命令。FXAA の各参照で色変換を繰り返す | 露出・白色点ゲインを画素ループ外へ移す。必要なら表示色の生成と FXAA を分離して比較 |
| 3 | CPU 頂点配列の reserve | 配列が空でも Teto 3体で10,353,152 bytes、5体で17,255,232 bytesの upstream 確保 | `rebuildVertices` 確定後、実際に頂点を生成する場合だけ reserve。UV morph/soft body/トポロジー変更を維持 |
| 別枠 | Bullet | 固定出力の対象 CPU 命令の83.54%が Bullet Dynamics/Collision 内 | substeps・接触・制約数を併記して計測。独立した物理 world の並列化などは依存関係と決定性を検証して別途判断 |
| 既存 | readback・停止中の再描画・材質 CPU 再構築 | 現在も一部残存。下記に既存 Issue と現行実装の差を整理 | #281/#282 の範囲で個別に測定・修正 |

優先度は実装の無駄の確かさ・適用条件・変更の大きさからの判断。
動画を表示しない場面では順位1の効果はない。確保量はメモリへの書込量、RSS、フレーム時間とは異なる。

## 対象と測定の限界

- main `f10efd8d0a9eaa87434676d659e65e98464d2f05`、libmmd `ca4ba1fb97c9cc30d9846d4f56cd5093c49bcebc`。
- `linux-release` を再 configure/build。GCC 16.2.1、`-O3 -DNDEBUG`。native/ThinLTO/PGO 無効、ImGui/物理/FFmpeg 有効。
- CPU: Ryzen 5 PRO 3500U、4 cores / 8 threads。GPU: Vega 8 / RADV RAVEN、Mesa 26.2.5-arch1.1。
- glslc 2026.4、Valgrind 3.25.1、FFmpeg n9.0.2。ELF 26,573,440 bytes、strip 前。
- ELF SHA-256: `181758d05d41c197afa6dd82630789fdd30f6439f8352576cf9dddd6aedb2a2a`。
- CPU/GPU クロック・温度は固定していない。他のユーザープロセスも動いていたため、時間値は性能保証や変更前後の因果比較に使わない。
- Callgrind の Ir は対象 CPU 区間で実行した命令数。SPIR-V/ISA の数は分岐の両側を含む静的命令数。相互に混同しない。
- `perf` は未導入。CPU のハードウェアカウンタ／サンプリングと GPU ハードウェアカウンタは未取得。
  実機 Subayai/BDPT、個々の FX、編集 UI 全体、DoF 有効時の性能も未測定。

## 1. 背景動画は毎フレーム GOP の先頭から読み直す

[MediaFile::decodeVideoFrame](../../src/core/media.cpp#L229) は毎回、
`av_seek_frame(AVSEEK_FLAG_BACKWARD)` → `avformat_flush` → `avcodec_flush_buffers` を実行し、
要求時刻に到達するまでパケットを送る。その後 `SwsContext` も生成・破棄する。
[refreshVideoFrame](../../src/app/application.cpp#L2802) の同一フレーム抑制はあるが、通常の動画再生では各新規フレームでこの経路を通る。

逆アセンブルでも `decodeVideoFrame` の本体986 bytesに、seek、flush、send/receive、
`sws_getContext` / `sws_freeContext` の実呼出しが残っている。
たとえば今回の ELF では `0x1cc1c4: call av_seek_frame@plt`、`0x1cc1ef: call avcodec_flush_buffers@plt`。

製品と同じ Release の `media.cpp.o` / `ffmpeg_utils.cpp.o` をリンクした probe で、
320×180、30 fps、lossless H.264、B-frame なしの先頭60フレームを順に要求した。
リンク時の `--wrap` は製品オブジェクトからの直接呼出しだけを計数する。

| 動画 | seek 回数 | send_packet 回数 | SwsContext 生成回数 | 60フレームの時間・3回（ms） |
| --- | ---: | ---: | ---: | --- |
| GOP 60 | 60 | 1,830 | 60 | 1849.08 / 2152.10 / 1410.35 |
| GOP 1 | 60 | 60 | 60 | 68.03 / 58.94 / 66.90 |

両動画の先頭60フレームを RGBA へデコードした13,824,000 bytesは一致した。
SHA-256: `8ae9351c9460b1399c979b38c6c095c7bd2858ce8a53712681c21f9ddce1d723`。
圧縮構造・ファイルサイズ・デコーダの仕事量も異なる比較なので、この時間比を「修正すれば何倍速くなる」と解釈しない。
1,830 = 1 + 2 + … + 60 という呼出し数が、連続再生でも読直しが増える直接の証拠。

最適化案は、直前にデコードした PTS と未消費のデコーダ状態を保持し、順送りを継続すること。
`AVPacket` / `AVFrame` / `SwsContext` も再利用できる。
フレーム寸法・pixel format の変更、逆送り、先頭へのループ、音声デコードとの demuxer 共有は明示的に無効化条件とする。
今回の probe の簡易 checksum は検証の代用ではなく、変更時には返却画像と要求 PTS を全体比較する必要がある。

## 2. トーンマップ／FXAA の計算量

[preview_tonemap.hlsl](../../shaders/preview_tonemap.hlsl#L59) の `displayAt` は参照ごとに
`applyExposureAndWhiteBalance` と view transform を実行する。
`outputAt` は透明領域の bloom にも色変換を適用する。
FXAA は中心・対角4点・エッジ追加4点の最大9地点について `outputAt` を呼ぶ。

`whiteBalanceGain` は [preview_color.hlsli](../../shaders/preview_color.hlsli#L99) にあり、
色温度から Planckian RGB、複数の除算、輝度正規化を計算する。
露出と色温度は画素に依存しないが、現在は値を push constant で渡してシェーダー内で求める。
これを CPU で設定変更時／フレーム時に求めた RGB ゲインへ置き換える候補がある。
GPU と CPU の演算差を許容する範囲を決め、露出・白色点の端点、ACES/AgX/Neutral、透明な bloom を比較する。

| glslc 出力 | bytes | 静的命令総数 | 画像 sample 箇所 | OpFDiv | Exp2 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 現行の `preview_tonemap.PS.spv` | 83,812 | 4,294 | 36 | 266 | 18 |
| 同じソースを `glslc -O` | 53,776 | 2,782 | 36 | 96 | 1 |

この差だけでは GPU の改善を主張できない。
[Mesa の公式デバッグフラグ](https://docs.mesa3d.org/envvars.html#radv-debug)でキャッシュを無効にして最終 ISA を取得し、
トーンマップに対応する fragment shader を比較した。

| RADV GFX9 出力 | code bytes | 静的命令 | SGPR / VGPR | spill / scratch | 報告上の subgroups/SIMD |
| --- | ---: | ---: | --- | --- | ---: |
| 現行 | 22,576 | 3,524 | 48 / 52 | 0 / 0 | 4 |
| `-O` の SPIR-V | 22,576 | 3,524 | 48 / 52 | 0 / 0 | 4 |

両方とも sample 箇所36、`v_rcp_f32_e32` 128、`v_exp_f32_e32` 55。
一部の多項式評価の命令が異なり、機械語は同一ではない。画像一致・フレーム時間の比較も行っていない。
**SPIR-V の小型化を根拠に `-O` を既定化する判断はしない**。
比較用に差し替えた生成 SPIR-V は元のハッシュへ復元済み。RADV の統計は実測 occupancy ではない。

表示色を一度作ってから FXAA で参照する構成なら周辺画素ごとの色変換を省けるが、
追加の attachment・pass・帯域が必要。特に APU では、画質を揃えた GPU 時間の比較が採否条件となる。
単純に HDR 色を補間してからトーンマップする置換は、非線形変換と alpha/bloom の結果を変えるため採用しない。

## 3. CPU 頂点を使わないフレームにも大きな reserve がある

[refreshAnimatedMesh](../../src/app/application.cpp#L2097) は `vertices.reserve(vertexCount)` を
無条件に呼ぶ。通常の GPU スキニングでは頂点生成を省く経路があるため、
`vertices.size() == 0` のままフレームを終えてもこの確保が発生する。

`PreviewVertex` は108 bytes、[FrameScratch](../../src/core/frame_scratch.hpp) の初期バッファは4 MiB。
現行 ELF でも `0xececa` の `imul $0x6c`、容量不足時の `0xf1cf2` の
`monotonic_buffer_resource::_M_new_buffer` 呼出しを確認した。
通常1体では初期バッファに入るため、ヒープ確保と scratch 内の確保を区別する必要がある。

デフォルト upstream を計数する probe に、製品と同じ `FrameScratch` と `PreviewVertex` を使った結果:

| Teto 個体数 | 頂点配列 size | reserve bytes | upstream 確保回数 | upstream bytes |
| --- | ---: | ---: | ---: | ---: |
| 1 | 0 | 3,451,032 | 0 | 0 |
| 3 | 0 | 10,353,096 | 1 | 10,353,152 |
| 5 | 0 | 17,255,160 | 1 | 17,255,232 |

`frameScratch_.reset()` は毎評価で upstream を解放する。
これは確保量を再現する小さな probe であり、実アプリ全体の malloc 回数・RSS・待ち時間の測定ではない。
確保をなくせることは確認できたが、FPS 改善量は未測定。
`evaluateInto` の後に `dynamicVertices` が追加で立つため、最初の `rebuildVertices` 判定だけで reserve を抑制すると不十分。
評価後の最終判定を使い、初期アップロード、UV/追加UV/一時頂点、soft body を含む経路を保持する。

## 4. CPU の主な仕事は物理ソルバーへ移っている

固定320×320の MP4 フレーム30/31について、`refreshAnimatedMesh` 内だけ Callgrind を収集。
preroll と初期アップロードを含む34回の評価で、393,156,439 Ir だった。
ロード・エンコード、フレーム外の UI、GPU の命令はこの区間に含まれない。

| 対象 | self Ir | 区間に対する比率 |
| --- | ---: | ---: |
| libBulletDynamics.so | 205,817,910 | 52.35% |
| libBulletCollision.so | 122,643,421 | 31.19% |
| `resolveSingleConstraintRowGeneric`（上記 Dynamics の内数） | 76,139,033 | 19.37% |
| `refreshAnimatedMesh` 本体 | 9,616,894 | 2.45% |
| `solveIk` 本体 | 8,440,461 | 2.15% |
| `evaluateInto` 本体 | 2,323,679 | 0.59% |

`stepSimulation` は124回、inclusive 337,436,139 Ir（85.83%）。
この inclusive 値と表の self 値は合算しない。ボーン補間だけを最適化しても区間全体の余地は限られる。
substep・solver 反復・衝突判定を減らせば接触や衣装の挙動が変わるので、単なる無駄として削除できない。

既存の [CPU 最適化の記録](optimization-callgrind.md)と整合し、
`makeNativeSceneModelData` のフレーム内呼出しは0、`makePreviewFrameVertex` は63,908回（初期アップロード2回分）。
出力2フレームの RGB 614,400 bytesも既存記録のハッシュ
`16d68dd6e2417ecd6502719f0c84b80410409142778ca589adfa65d572f7e705` と一致した。
解消済みの native データ構築・全頂点変換を、新規の最適化候補として扱わない。

## 5. 実際の Preview の観測値

Teto + telephone_merged.vmd、1/3/5体、Preview/APU、hidden/immediate、validation 無効。
各条件60フレーム warmup 後、300フレームを3回。各列は3回の中央値、単位 ms。
window 1280×720、描画 viewport は733×271。1080p 出力の測定とは区別する。

| 個体数 | CPU animation | CPU render | GPU全体 | GPU shadow | GPU depth/AO | GPU materials | GPU tonemap | フレーム p95 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1.07 | 1.27 | 2.76 | 0.177 | 0.402 | 0.571 | 0.538 | 3.92 |
| 3 | 3.31 | 0.85 | 4.50 | 0.461 | 0.705 | 1.307 | 0.558 | 6.76 |
| 5 | 9.04 | 0.74 | 7.36 | 0.882 | 1.177 | 2.408 | 0.682 | 13.46 |

CPU/GPU は重なるので合算しない。GPU 時間は非同期のタイムスタンプ取得値。
実時間で進むモーションと適応物理の評価回数は条件間で固定していない。
従って個体数ごとの差を単一実装のスケーリング率と断定しない。
一体の GPU 区間ではトーンマップの比重が見える一方、複数体では materials と物理の優先度が上がる。

## 6. 既存 Issue の再確認と未計測の候補

- [#281](https://github.com/haturatu/mikumikudesu/issues/281): Preview 連番画像では現在も
  `renderToImage` の LDR 結果を使わず、その後 HDR readback を行う。
  [advanceImageSequenceExport](../../src/app/application.cpp#L4510) の不要な LDR 転送と、
  [readbackTextureSubresources](../../src/graphics/vulkan/vulkan_device.cpp#L7948) の都度 staging 確保は別に扱う。
  `collectRenderedImage` には画素 vector の初期化後の memcpy と BGRA の全画素 swap が残る。
  1080p の RGBA は8,294,400 bytes/frame。CPU 時間・実帯域は今回未測定。
  CLI 動画は既に3枚の readback ring を使い、GUI 動画には非待機の回収経路もある。
  毎フレーム必ず device-wide idle しているという指摘には該当しない。
- [#282](https://github.com/haturatu/mikumikudesu/issues/282): 停止中も描画ループは動き、
  deform/shadow/AO/SSS/bloom/tonemap の再実行を抑える余地がある。
  `deformRecorded` は同一フレーム内の重複を防ぐ値で、次フレームの姿勢不変キャッシュではない。
  前フレームを参照する背景、時間依存 FX、temporal/カメラ/ライト/サイズ変更を含む無効化が必要。
  今回は停止中専用の実測を行っていない。
- #282 の「deform descriptor の毎フレーム書換え」は現行では
  [bindingsChanged](../../src/graphics/vulkan/vulkan_device.cpp#L4704) の比較で抑制済み。
  材質の GPU 転送にも dirty range と世代判定がある。
  CPU 側の材質変換・テーブル構築が残ることと、毎回 GPU 全転送することを混同しない。
- [#279](https://github.com/haturatu/mikumikudesu/issues/279)、[#278](https://github.com/haturatu/mikumikudesu/issues/278):
  FX の再構築や native BLAS/TLAS は今回の Preview 実測の対象外。
- UV/追加UV morph を持つモデルは、その morph が不活性でも `requiresCpuVertices` が立つ。
  GPU 経路で CPU 頂点の copy/normalize/upload を再発させるモデル条件として追加計測の価値がある。
  有効な group/flip/temporary morph と soft body を考慮する必要があり、今回の Teto の結果から改善量は出せない。
- limb debug log は Release にもあり、30フレームごとにボーン名検索・補間・stdout flush を行う。
  ただし今回のプロファイルでは支配項ではない。レベル設定で遅延評価するなら DEBUG/INFO→stdout、WARN/ERROR→stderr の規約を保つ。

## 再現

必要ツール: CMake/Ninja、通常のビルド依存、GNU binutils、SPIR-V Tools、glslc、Valgrind、FFmpeg、pkg-config、Python 3.10以降。
以下は repo root で実行する。`audit_dir` は未使用のディレクトリにする。

```sh
audit_dir=$(mktemp -d /tmp/mikumikudesu-audit.XXXXXX)
cmake --preset linux-release
cmake --build --preset linux-release --target mikumikudesu -j3
python scripts/audit-render-binaries.py --binary build/linux-release/mikumikudesu \
  --shaders build/linux-release/shaders --output "$audit_dir/static"

python scripts/benchmark-preview.py --binary build/linux-release/mikumikudesu \
  --model assets/models/teto/model/teto.pmx --motion telephone_merged.vmd \
  --models 1 3 5 --frames 300 --runs 3 --output "$audit_dir/playback"

DEBUGINFOD_URLS= XDG_CONFIG_HOME="$audit_dir/config" \
  valgrind --tool=callgrind --collect-atstart=no \
  --toggle-collect='dayo::app::Application::refreshAnimatedMesh*' \
  --callgrind-out-file="$audit_dir/animation.callgrind" \
  build/linux-release/mikumikudesu --hidden --no-validation --preview-quality apu \
  --asset assets/models/teto/model/teto.pmx --asset telephone_merged.vmd \
  --export-video "$audit_dir/fixed.mp4" --video-width 320 --video-height 320 \
  --video-fps 30 --video-from-frame 30 --video-to-frame 31 --no-audio
callgrind_annotate --auto=no --inclusive=no --show=Ir --threshold=100 "$audit_dir/animation.callgrind"

# ISA 採取は時間計測とは別に実行する。既定 SPIR-V を変えない。
RADV_DEBUG=ps,asm,shaderstats,nocache MESA_SHADER_CACHE_DISABLE=true \
  XDG_CONFIG_HOME="$audit_dir/config" build/linux-release/mikumikudesu \
  --hidden --no-validation --preview-quality apu \
  --asset assets/models/teto/model/teto.pmx --frames 3 \
  > "$audit_dir/gpu.stdout" 2> "$audit_dir/gpu-isa.log"
glslc -O -x hlsl -DDAYO_GLSLC=1 --target-env=vulkan1.3 -fshader-stage=frag \
  -fentry-point=PS shaders/preview_tonemap.hlsl -o "$audit_dir/tonemap-opt.spv"
spirv-dis "$audit_dir/tonemap-opt.spv" -o "$audit_dir/tonemap-opt.spvasm"

g++ -std=c++20 -O3 -DNDEBUG -Isrc docs/performance/frame_scratch_probe.cpp \
  -o "$audit_dir/frame_scratch_probe"
"$audit_dir/frame_scratch_probe"

g++ -std=c++20 -O3 -DNDEBUG -Isrc docs/performance/media_decode_probe.cpp \
  build/linux-release/CMakeFiles/dayo_core.dir/src/core/media.cpp.o \
  build/linux-release/CMakeFiles/dayo_core.dir/src/core/ffmpeg_utils.cpp.o \
  -Wl,--wrap=av_seek_frame -Wl,--wrap=avcodec_send_packet -Wl,--wrap=sws_getContext \
  -o "$audit_dir/media_decode_probe" \
  $(pkg-config --cflags --libs sdl3 libavcodec libavformat libavutil libswresample libswscale)
for gop in 60 1; do
  ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=320x180:rate=30 \
    -frames:v 90 -c:v libx264 -preset fast -crf 0 -g "$gop" -keyint_min "$gop" \
    -sc_threshold 0 -bf 0 "$audit_dir/gop$gop.mp4"
done
for run in 1 2 3; do
  for gop in 60 1; do
    "$audit_dir/media_decode_probe" "$audit_dir/gop$gop.mp4"
  done
done
```

比較用 ISA を再取得する場合は、別ビルドツリーの `CompileHlsl.cmake` に `-O` を加えて同条件で採取する。
本調査では生成済みファイルを一時差替えし、`finally` で元の bytes を復元して比較した。
素の SPIR-V だけを `spirv-dis` した結果と、RADV の ISA を区別する。
再生・ISA 採取は一時ディレクトリを cwd にして実施し、既存の `imgui.ini` / `pipeline.cache` を保護した。

検証: Release 再ビルド成功、実 GPU で9回の再生計測、固定2フレームの出力・既存ハッシュ一致、
両 probe のコンパイルと実行、動画入力の全60フレーム RGBA 一致、収集スクリプト実行・Python lint、C++ format を確認。
製品コードを変更していないため、CTest 一式の再実行は行っていない。
