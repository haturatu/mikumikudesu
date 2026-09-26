# 外部テクスチャのメタデータとdispatch

調査基準: MikuMikuDayo130、`src/YRZFx.ixx` の `FXTexture`、`createTextureLambda`、
`createTexture3DLambda`。ファイルを指定した場合、本家は宣言のsize/formatを無視し、読込結果を使う。

## 問題と修正

現行runtimeはsizeが書かれていると画像寸法との一致を要求し、formatもRGBA8以外なら拒否していた。
また `FxCompiler::plan` はGPUにロードされた画像寸法を使わず宣言から再計算するため、画像名を
outputSize.baseに指定したcomputeのdispatchや、その画像を基準にしたリソースと不一致が生じた。

- 外部2D/3D画像の実寸を採用し、宣言のsize/formatは無視する。
- 既存decoderの出力RGBA8にshaderのfloat4宣言とattachment formatを合わせる。
- nativeの初期化・各frameのplanにphysical resource tableを渡す。pipeline生成はGPU確保後にplanする。
- physical tableを持たないメタデータ解析用planは従来の宣言からの推定を維持する。

## 確認と限界

`fx_executor`で2D画像の競合size/unknown format、3D DDSの未定義size.base/競合format、
派生texture、明示/暗黙のdispatch寸法を確認する。実画像のDXGI形式保存は別件で、
本変更の外部画像は従来どおりRGBA8へdecodeする。HDR/float DDSの精度互換を解決したものではない。
宣言順を越えるsize.base参照の依存解決は別の未対応項目。
