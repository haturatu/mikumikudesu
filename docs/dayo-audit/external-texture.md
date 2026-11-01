# 外部テクスチャのメタデータとdispatch

外部画像を読み込む FX では、宣言したサイズとファイルの実際のサイズが違うことがあります。ここを宣言値だけで処理すると、後続の dispatch や出力サイズまでずれてしまいます。

調査基準: MikuMikuDayo130、`src/YRZFx.ixx` の `FXTexture`、`createTextureLambda`、
`createTexture3DLambda`。ファイルを指定した場合、本家は宣言のsize/formatを無視し、読込結果を使う。

## 調査時点の問題と修正内容

上流が外部画像の実寸を使う場合、Vulkan 側の plan もその実寸に合わせる必要があります。

修正前は、FX の宣言に size があると画像ファイルの実寸との一致を要求し、format が RGBA8 以外なら拒否していました。

さらに `FxCompiler::plan` は GPU に読み込まれた画像の寸法ではなく宣言値から再計算していました。そのため、外部画像を `outputSize.base` にした compute の dispatch や、その画像を基準にするリソースでサイズが食い違っていました。

- 外部2D/3D画像の実寸を採用し、宣言のsize/formatは無視する。
- 既存decoderの出力RGBA8にshaderのfloat4宣言とattachment formatを合わせる。
- nativeの初期化・各frameのplanにphysical resource tableを渡す。pipeline生成はGPU確保後にplanする。
- physical tableを持たないメタデータ解析用planは従来の宣言からの推定を維持する。

## 確認した内容と制限事項

`fx_executor` では、2D 画像の競合する size / 不明な format、3D DDS の未定義 `size.base`、派生テクスチャ、明示・暗黙の dispatch サイズを確認します。

ただし、読み込んだ画像は従来どおり RGBA8 へ decode します。HDR / float DDS の精度互換性や、宣言順をまたぐ `size.base` の依存解決まで修正したものではありません。
