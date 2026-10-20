# CPU最適化のCallgrind比較

基準は#310 `294175a`、変更後は#309まで（#285/#306/#305/#304/#307/#308/#309を含む）。
同じGCC Release、Teto+telephone_merged.vmd、Preview/APU、320×320動画出力のフレーム30/31を比較した。
出力開始までの固定フレームwarm-upも含み、両方ともrefreshAnimatedMeshが34回呼ばれた。
`--collect-atstart=no --toggle-collect='dayo::app::Application::refreshAnimatedMesh*'`で対象関数内だけ収集。
DDS読み込み・エンコード時間をフレーム最適化の効果に含めない。

| 関数/処理 | 前 self Ir | 後 self Ir | 前/後 呼出し |
| --- | ---: | ---: | ---: |
| refreshAnimatedMesh | 141,186,956 | 9,616,894 | 34 / 34 |
| makeNativeSceneModelData | 282,829,170 | 0 | 34 / 0 |
| 上記のtangent計算（vertices/rawVertices） | 379,148,688 | 0 | 68 / 0 |
| MmdAnimator::evaluate → evaluateInto | 8,533,861 | 2,323,679 | 34 / 34 |
| expandMorphWeights → MorphWeightExpander::expandInto | 259,726 | 69,221 | 34 / 34 |
| normalizeForPreview | 10,052,236 | 986 | 34 / 34 |

Native構築/そのtangentは変更後のprofileに現れなかった。
頂点変換の呼出しはmakePreviewVertex 1,150,344回からmakePreviewFrameVertex 63,908回へ減った。
後者は初回アップロード2回分の31,954頂点で、Preview中のnative変換を除いた結果と整合する。

収集区間全体のIrは1,389,860,402→393,155,940。ただし関数分割・インライン展開・allocator・driverも含む。
表はcallgrind_annotate `--inclusive=no --show=Ir --auto=no --threshold=100`のself値で、
親と子のinclusive値は合算しない。複数変更を含む比較なので、全体差を一つの変更へ帰属させない。
Irは機械命令数の計測値で、実時間、GPU時間、FPSの改善率へ換算できない。

通常実行とCallgrind実行の両方で、比較した2フレームをデコードしたRGB 614,400 byteは完全一致した。
SHA-256: `16d68dd6e2417ecd6502719f0c84b80410409142778ca589adfa65d572f7e705`。

## 再現

別worktreeで同じコンパイラ・設定の基準バイナリを作り、同じコマンドを新旧へ適用する。
configを分離し、入力パス・from/to・画面サイズをそろえる。以下は1条件分。

```sh
XDG_CONFIG_HOME=/tmp/callgrind-config valgrind --tool=callgrind \
  --collect-atstart=no --toggle-collect='dayo::app::Application::refreshAnimatedMesh*' \
  --callgrind-out-file=/tmp/preview.callgrind \
  build/linux-release/mikumikudesu --hidden --no-validation --renderer preview --preview-quality apu \
  --asset assets/models/teto/model/teto.pmx --asset telephone_merged.vmd \
  --export-video /tmp/preview.mp4 --video-width 320 --video-height 320 --video-fps 30 \
  --video-from-frame 30 --video-to-frame 31 --no-audio --overwrite
callgrind_annotate --auto=no --inclusive=no --show=Ir --threshold=100 /tmp/preview.callgrind
callgrind_annotate --auto=no --inclusive=no --tree=both --threshold=100 /tmp/preview.callgrind
```
