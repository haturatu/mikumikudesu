# #304: 通常GPUスキニングの頂点コピーを省略する

libmmd PR https://github.com/haturatu/libmmd/pull/17 に、明示的な`VertexOutput::omitUnchangedGpu`を追加。
既定のAPIは従来どおりfull。CPUスキニング、UV/追加UVモーフを持つモデル、soft body、一時頂点編集はfullを維持する。
通常のGPUスキニングはボーン・材質・頂点モーフウェイト・表示・物理を評価し、CPU頂点配列だけを省く。

Previewの初回/構成変更とNative切替では、必要なbind属性を元モデルから変換する。
元モデルの位置だけをその場で正規化し、既に正規化済みのfull出力と二重に変換しない。
UVモーフをCPU出力する場合は毎フレームの頂点アップロードも行う。

## CPU単体比較

GCC Release / Vega 8ホスト、Teto 31,954頂点 + telephone_merged.vmd。
同一バイナリでfullとomitを交互に各3回、各310評価の先頭10回を除く300回を集計。
固定時刻0.5フレーム刻み、GPU skinning、物理なし、evaluate + normalizeを測定する。
`animation_benchmark.cpp`は測定に使ったソース。公開API既定のfullとopt-inを比較できる。

```sh
c++ -O3 -std=c++20 -I external/libmmd/include docs/performance/animation_benchmark.cpp \
  build/linux-release/external/libmmd/libmmd_animation.a \
  build/linux-release/external/libmmd/libmmd_physics.a \
  build/linux-release/external/libmmd/libmmd_core.a \
  -lBulletDynamics -lBulletCollision -lLinearMath -o /tmp/animation-bench
/tmp/animation-bench assets/models/teto/model/teto.pmx telephone_merged.vmd
```

| モード | 平均msの3回中央値 | p95 msの3回中央値 | CPU出力頂点数 | 頂点配列byte |
| --- | --- | --- | --- | --- |
| full | 0.8390 | 0.9178 | 31,954 | 5,496,088 |
| omit | 0.0880 | 0.1148 | 0 | 0 |

これは物理なしのCPU単体比較であり、通常再生のFPS向上率ではない。

## アプリの再生計測

APU品質、1280×720ウィンドウ、Viewport 732×457、300フレーム×3回。
下表は各3回の中央値ms。beforeは#305までのバイナリ、afterは本変更。
GPUクロックと壁時計で進むVMD/物理は固定せず、区間時間とフレーム周期を加算しない。

| モデル数 | animation前/後 | GPU前/後 | p95前/後 |
| --- | --- | --- | --- |
| 1 | 2.25 / 1.17 | 3.95 / 3.03 | 4.67 / 4.03 |
| 3 | 8.35 / 2.95 | 6.66 / 4.99 | 10.42 / 5.98 |
| 5 | 13.19 / 4.61 | 9.43 / 6.74 | 17.58 / 8.47 |

## 検証

libmmd Debug 14テスト、アプリRelease有効31テスト成功。
UVと追加UV全チャンネル、一時頂点、CPU/default/soft bodyのfullフォールバックを確認。
省略時のbind属性変換をBDEF/SDEF/QDEFと非単位の正規化で比較。
アプリテストでUVのフレーム間更新・GPUへの転送、Preview→Native時の頂点復元と物理再評価なしを確認。
基準#310と#305までのTeto+VMDの320×320・フレーム30/31出力はデコード後RGBが完全一致。
本変更も同じRGB SHA-256 `16d68dd6e2417ecd6502719f0c84b80410409142778ca589adfa65d572f7e705`で一致した。Native RT実機比較はVega 8非対応のため未実施。
