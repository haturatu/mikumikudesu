# 調査: FX Debugのpreview/dump接続

状態: **未実装・調査PR**。基準は `main@7ed41e0` と固定 MikuMikuDayo130。

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

今回の確認はソース比較。preview/dumpの実装・GPU検証は含まない。
