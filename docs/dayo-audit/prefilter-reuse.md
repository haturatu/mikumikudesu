# 環境光 prefilterの上流shader再利用

環境光の prefilter を独自シェーダーで複製すると、上流を更新したときに処理の差が増えていきます。なので、可能な部分は固定した上流のシェーダーをそのまま再利用する方針にしました。

調査基準: 固定 MikuMikuDayo130、`src/YRZ.ixx::PrefilterShader` と `DXR::PrefilterEnvmap`。

## 調査時点の問題

同じ計算を自前で持ってしまうと、配布物を更新しても古い実装が残ります。

`NativeDayoEnvironmentRuntime`はPCG4d、VNDF、BRDF等を含むshader約100行をC++文字列へコピーしていた。
上流ZIPを更新しても同コードは変わらず、実際には存在しない `system/skyboxPrefilter.hlsl` を
compile診断上のパスに使っていた。

## 修正内容

この修正では `hlsl/../src/YRZ.ixx` から `PrefilterShader` のraw literalをそのまま読み込む。
Vulkanのvertex bufferなし描画に必要な `NativePrefilterVS` だけ追加し、元のVSを呼ぶ。
PS/PSCopyとsample helperは上流sourceをそのまま使う。上流ソースの欠損・空literal・終端欠損・
宣言重複を診断する。任意のC++を実行/compileする処理ではなく、固定した文字列の読込である。

runtime manifestへ `src/YRZ.ixx` を追加し、fetchの存在検査とCMake installの対象にする。
従来のlicenceディレクトリの配布は継続する。既存の4 iteration、64 samples、mipとblendの設定は維持する。

## 検証と制限事項

`subayai_bdpt`はliteral保持とadapter、変更されたsourceの反映、欠損/不正sourceを検証し、
Dayo/DXCが利用可能な場合は本家shaderの3 entryのSPIR-V生成とmip別draw記録を検証する。
`upstream_fetch`はmodule欠損時のstale判定とmoduleを含むpackage内容を検証する。

上流raw literalの宣言形式が変わった場合はadapterの更新が必要。実行中のYRZ.ixx編集のhot reloadや、
Windows 版 1.30 との GPU 画像同等性は、この修正では検証していません。
