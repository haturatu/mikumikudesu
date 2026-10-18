# FX バッファのファイル初期化

調査基準: `deps/mikumikudayo.lock` の MikuMikuDayo130。
`MikuMikuDayo/src/YRZFx.ixx` の `FXBuffer::filename` と `createBufferLambda` を照合した。

## 調査時点の問題と修正内容

`buffers[].filename` が `EffectBuffer` に保持されず、GPUバッファの初期データが失われていた。
parser → FxProgram → FxResourceRuntime にファイル名を通し、effectディレクトリを基準に読み込んで
既存の `Device::uploadBufferEx` に接続する。Dayoと同様に確保サイズ分だけ読み、短いファイルは
残りをゼロ、長いファイルは超過分を無視する。開けないファイルはパス付きで初期化失敗とする。

GPU 所有権を store へ登録してから I/O を行うため、読込やuploadの失敗時にもresetがバッファを解放する。
D3D12に依存する上流C++を直接リンクせず、上流の入出力契約を既存Vulkan uploadへ接続した。

## 検証範囲

`fx_executor` の回帰ケースでparser/compileの保持、3/8/12 byteのファイルから8 byteへのupload、
相対パス、欠損ファイルの診断と解放を確認する。MockDevice を使う検証であり、Windows 版と GPU 出力を比較した結果ではありません。
`shared=ref` による参照元の初期化は別の未対応項目。
