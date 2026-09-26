# 調査: FX Debugのpreview/dump接続

状態: **実装済み**。基準は `main@7ed41e0` と固定 MikuMikuDayo130。

本家 `src/debDayo.h::DebugImageProcessor` は `hlsl/system/debug.hlsl` を使い、選択したtextureを
表示用画像へ変換する。同ファイルの `dumpLambda` はbuffer download、2D/3D texture保存を行い、
UIからdumpと画像表示を提供する。

[ApplicationのFX Debug](../../src/app/application.cpp) は宣言metadataと
`NativeRendererCoordinator::liveResources()` の実allocation情報を表示する。
ここにpreview/dump要求やreadback処理はない。snapshotへGPU handleを公開しない設計なので、
既存snapshotにImGui画像表示を追加するだけでは本家機能にならない。

対応はeffect owner/generation/resource名を使ったdebug要求をrender threadへ送り、
GPU完了後にreadback結果を返す経路とする。texture previewのchannel/range変換には
本家debug.hlslをbindし、D3D12専用のDebugImageProcessorはVulkan resource ownerへ置き換える。
bufferはraw bytes、textureはmip/array/depth sliceとformatを明示して保存する。

受け入れ条件は、2D/3D/float/depth/bufferの実データ照合、mip/slice選択、保存失敗の通知、
readback待機中のeffect削除/reload/resize、frame in flight下の寿命管理。
既存Deviceのreadback APIの存在だけで対応済みとはしない。

## 実装

FX Debug UIからowner/resource/generationとmip/slice/mode/scaleを指定して要求を送る。
frameのeffect実行後にgenerationを再確認し、command listのGPU完了境界でreadbackする。
削除・reload・resize済みの要求を拒否し、UIにはCPU snapshotだけを返す。

textureの数値形式をfloat32表示入力へ変換し、上流 `system/debug.hlsl` のPSをcompute wrapperから呼ぶ。
checker、RGB、各channelのhue、scaleは上流処理を再利用。UIは最大128×128の縮小表示を行う。
bufferは先頭256bytesのhex表示と `.bin` 保存。device-local bufferのVulkan staging readbackも追加した。
textureは選択sliceのPNG/EXR、または選択mip全体のraw `.bin` とformat/寸法JSONを保存する。
FX allocationにはarrayがないためarray layerは0。readbackとfloat展開はそれぞれ256 MiBまでに制限する。

既存ファイルは上書きせず、IO/compile/readbackエラーをUIへ返す。明示要求時は同期readbackで描画を待機する。
Linux `mikumikudesu` ビルド成功。今回GPU実行・テストは行っていない。前提PR: #248。
