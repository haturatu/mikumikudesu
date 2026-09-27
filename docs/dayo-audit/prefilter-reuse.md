# 環境光prefilterの上流shader再利用

調査基準: 固定 MikuMikuDayo130、`src/YRZ.ixx::PrefilterShader` と `DXR::PrefilterEnvmap`。

`NativeDayoEnvironmentRuntime`はPCG4d、VNDF、BRDF等を含むshader約100行をC++文字列へコピーしていた。
上流ZIPを更新しても同コードは変わらず、実際には存在しない `system/skyboxPrefilter.hlsl` を
compile診断上のパスに使っていた。

本変更では `hlsl/../src/YRZ.ixx` から `PrefilterShader` のraw literalをそのまま読み込む。
Vulkanのvertex bufferなし描画に必要な `NativePrefilterVS` だけ追加し、元のVSを呼ぶ。
PS/PSCopyとsample helperは上流sourceをそのまま使う。上流ソースの欠損・空literal・終端欠損・
宣言重複を診断する。任意のC++を実行/compileする処理ではなく、固定した文字列の読込である。

runtime manifestへ `src/YRZ.ixx` を追加し、fetchの存在検査とCMake installの対象にする。
従来のlicenceディレクトリの配布は継続する。既存の4 iteration、64 samples、mipとblendの設定は維持する。

`subayai_bdpt`はliteral保持とadapter、変更されたsourceの反映、欠損/不正sourceを検証し、
Dayo/DXCが利用可能な場合は本家shaderの3 entryのSPIR-V生成とmip別draw記録を検証する。
`upstream_fetch`はmodule欠損時のstale判定とmoduleを含むpackage内容を検証する。

上流raw literalの宣言形式が変わった場合はadapterの更新が必要。実行中のYRZ.ixx編集のhot reloadや、
Windows 1.30とのGPU画像同等性はこの変更の確認範囲外。
