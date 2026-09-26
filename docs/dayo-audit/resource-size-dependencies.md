# 調査: FXリソースサイズの依存解決と最小サイズ

状態: **未実装・調査PR**。基準は `main@7ed41e0` と固定 MikuMikuDayo130。

本家 `src/YRZFx.ixx` のリソース生成は、絶対サイズ・外部ファイル・shared refを先に解決し、
残りのtexturemap/texture3Dmap/buffermapからsize.baseの依存先を辿る。
`FXSize::convert` は相対サイズの丸め結果を `max(1ull, ...)` にする。

[FxResourceRuntime::initialize](../../src/graphics/fx_resource_runtime.cpp) と
[FxCompiler::plan](../../src/fx/fx_compiler.cpp) はtextures→textures3D→buffersを1回ずつ走査し、
その時点のtableだけでsizeを解決する。後に宣言された同種リソースや後続の別種リソースをbaseに
する入力は解決できない。plannerとallocatorでサイズ変換コードが重複している。
[FxSizeResolver](../../src/core/fx/fx_size.cpp) は正のratioでも丸めて0になれば例外にする。
例: base幅1、ratio.x=0.5、truncは本家では1、現行ではエラー。

## 対応案と受け入れ条件

- 共通の依存グラフresolverへ変換処理を集約し、宣言順に依存せず解決する。
- 未定義baseと循環を区別し、依存chainをエラーに表示する。
- externalの実寸をterminal nodeに使う。shared=refをsize.baseにする上流禁止条件も確認する。
- 相対サイズの下限1を再現し、NaN/Inf、overflow、allocation budget検証は維持する。
- 前方参照、2D→3D→bufferの混在、循環、自分自身、ratioで0になる小さいrender targetを検証する。
- allocatorの実寸・shader定数・compute group・raster viewportを同じ解決結果から作る。

外部画像のsize/format優先順位の修正とは独立した残件。本PRはソース比較と受け入れ条件のみで、
依存resolverや下限丸めを実装していない。
