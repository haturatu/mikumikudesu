# FX バッファのファイル初期化

FX の buffer にファイル名が指定されている場合、GPU メモリだけ確保しても元データが入っていなければ意味がありません。上流の初期化処理に合わせ、ファイル読み込みと upload の扱いを確認しました。

調査基準: `deps/mikumikudayo.lock` の MikuMikuDayo130。
`MikuMikuDayo/src/YRZFx.ixx` の `FXBuffer::filename` と `createBufferLambda` を照合した。

## 調査時点の問題と修正内容

読み込みに失敗した場合も GPU 側のバッファが残らないよう、リソースの所有権と初期化の順序を整理しています。

修正前は `buffers[].filename` が `EffectBuffer` に保持されておらず、GPU バッファにファイルの初期データが入っていませんでした。

修正では parser → FxProgram → FxResourceRuntime にファイル名を渡し、effect ディレクトリから読み込んだデータを `Device::uploadBufferEx` へ渡します。上流と同じように確保したサイズまで読み、足りなければゼロで埋め、超えた分は無視します。ファイルが開けなければパスを含むエラーで初期化を止めます。

ファイルの読み込みより先に GPU リソースの所有権を store に登録しているため、I/O や upload が失敗しても reset 時に解放できます。

D3D12 固有の上流 C++ をそのままリンクするのではなく、ファイル初期化の動作を既存の Vulkan upload へ合わせています。

## 検証範囲

`fx_executor` の回帰ケースでparser/compileの保持、3/8/12 byteのファイルから8 byteへのupload、
相対パス、欠損ファイルの診断と解放を確認する。MockDevice を使う検証であり、Windows 版と GPU 出力を比較した結果ではありません。
`shared=ref` による参照元の初期化は別の未対応項目。
