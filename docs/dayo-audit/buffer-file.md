# FXバッファのファイル初期化

調査基準: `deps/mikumikudayo.lock` の MikuMikuDayo130。
`MikuMikuDayo/src/YRZFx.ixx` の `FXBuffer::filename` と `createBufferLambda` を照合した。

## 問題と修正

`buffers[].filename` が `EffectBuffer` に保持されず、GPUバッファの初期データが失われていた。
parser → FxProgram → FxResourceRuntime にファイル名を通し、effectディレクトリを基準に読み込んで
既存の `Device::uploadBufferEx` に接続する。Dayoと同様に確保サイズ分だけ読み、短いファイルは
残りをゼロ、長いファイルは超過分を無視する。開けないファイルはパス付きで初期化失敗とする。

GPU所有権をstoreへ登録してからI/Oを行うため、読込やuploadの失敗時にもresetがバッファを解放する。
D3D12に依存する上流C++を直接リンクせず、上流の入出力契約を既存Vulkan uploadへ接続した。

## 確認

`fx_executor` の回帰ケースでparser/compileの保持、3/8/12 byteのファイルから8 byteへのupload、
相対パス、欠損ファイルの診断と解放を確認する。MockDeviceによる検証であり、Windows本家とのGPU比較ではない。
`shared=ref` による参照元の初期化は別の未対応項目。
