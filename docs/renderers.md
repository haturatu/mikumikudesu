# レンダラ

描画方式は `--renderer preview|subayai|bdpt` で指定します。既定値は `preview` です。

## 対応状況

| レンダラ | 利用条件 | 備考 |
| --- | --- | --- |
| Preview | Vulkan 1.3、swapchain 対応 GPU | 通常のプレビュー、動画書き出し |
| Subayai | RT 対応 GPU、FX graph の要求 feature、native runtime の初期化 | 条件を満たさない場合は Preview に戻る |
| BDPT | RT 対応 GPU、FX graph の要求 feature、native runtime の初期化 | すべての FX の動作や結果の一致は未検証 |

```sh
./build/linux-debug/mikumikudesu --probe --hidden
./build/linux-debug/mikumikudesu --renderer preview --preview-quality auto --asset model.pmx
```

## 制限事項

PreviewはLinux/AMDで実動します。SubayaiとBDPTについては、`.fxdayo`のraygen/miss/hit-groupを
保持するFX契約、DXC→SPIR-V shader compiler、typed Vulkan resource/descriptor/pipeline/SBT、
deform→BLAS/TLAS、environment/light GPU resource、native runtime、OpenEXR encoderまでを
段階的に実装しています。

Applicationのswapchain、viewport、offscreen、画像連番出力はnative rendererの出力bridgeに接続されています。
ただし`nativeSubayai`と`nativeBdpt`は常に有効になるわけではありません。起動時にGPU capability、
FX graphが要求するfeature、runtimeの初期化結果を確認し、いずれかが不足している場合は理由を記録して
Previewへ戻します。要求featureはrenderer名だけでなく、各グラフの実際の宣言から判定されます。
そのためnative pathが接続済みであることと、すべてのGPU・すべての`.fxdayo`でnative描画できることは
別の契約です。対応外の環境では従来どおりPreviewを使用します。

Subayaiの材質注釈はコア層から専用GPU ABIへリンクされ、標準の`hair.txt`に含まれる異方性、IOR、
AutoNormalをPreview ABIと分離して保持します。Preview側のPBRプリセットはPMX材質ごとに指定します。

## Previewの画質設定

PreviewはRT機能を使わず、方向光の1024²/2048²シャドウマップ、comparison PCF、GGX直接反射、SH拡散IBL、
GGXでプリフィルタした環境キューブ、圧縮法線プリパス、scalar SSAO、RGBA16F合成と
ACESトーンマップを使用します。BRDF積分にはLUTの代わりに近似式を使います。
HDRIのプリフィルタは画像の読み込みまたは更新時に計算します。GPUへ送る照明HDRIは
最大2048×1024、環境キューブは最大512²で、元画像を線形色の面積平均で縮小します。
環境用RGBA16Fテクスチャ3枚の合計は最大約44 MiBです（デコード時のCPUメモリやstagingは別）。
SHは元画像から投影し、EnvironmentDesc::exposureはSHと照明画像に同じRGB倍率を適用します。

SceneをInspectorで選択すると、背景画像とは別に2:1のequirectangular HDRIを照明に指定できます。
HDRIを指定しない場合、2:1の背景画像を照明に使用します。モデルのInspectorでは材質ごとに
PMX、Skin、Hair、Cloth、Metal、Plastic、Glassを選べます。これらの選択はプロジェクトに保存されます。

画像連番出力ではPreview render scaleを1x、1.33x、2xから選び、Samplesを増やすと
Haltonジッタを用いてRGBA16Fの結果を平均します。平均後に出力解像度へ縮小し、ACESで表示用画像へ
変換します。APU profileではhardware PCF 1 tap、highでは3×3、静止画出力では7×7 comparison PCFを使用します。

`--preview-quality auto|apu|high`で画質を選べます。既定のautoはintegrated GPUとmemory budgetを考慮します。
Morph/skinningはframeごとのcompute結果を共有し、動画出力は3本のreadback ringを使います。
Present mode、experimental async compute、HDRI cacheと測定方法は[Preview performance](preview-performance.md)を参照してください。

画像連番出力はOpenEXRが利用できるbuildではEXRをエンコードし、利用できない場合は従来どおり
PNG/PPMを使います。Subayai/BDPTが選択されている場合は、viewport、offscreen render、画像連番出力が
同じnative frame recorderの結果を使用します。native runtimeを利用できない場合は、renderer選択時に
Previewへフォールバックします。
`.vmdayo`は本家1.30ソースのv3レイアウト（header、model dictionary、metadata、全track、
axis別MMD/Catmull-Rom方式）を実装しています。単体ファイルの二重headerと`.dayo`内の単一headerを
区別し、カメラsubsetとモデル別subsetを双方向変換します。未知の入力はopaque payloadとして保持します。
本家Windowsバイナリが生成したfixtureによる相互運用テストは、fixtureを同梱していないため未実施です。

## シェーダーと依存

`cmake/CompileHlsl.cmake`はDXCがあればVulkan 1.3向けSPIR-Vを生成し、Preview shaderは
glslc HLSL frontendへfallbackします。既存のbindless/ray query/ray tracing shaderを移す場合は
DXCが必須です。`SV_Barycentrics`はSPIR-V fragment barycentricへ写像する設計です。

`vcpkg.json`にはSDL3、Vulkan、DXC、DirectXTex、Bullet、Dear ImGui、ImGuizmo、cereal、
Jsonnetを宣言しています。Linux実行系の画像読込は現在stbと内蔵DDS decoderを使うため、
WICには依存しません。OIDNは任意で、HIP deviceが使えなければCPUへfallbackします。

## 関連項目

- [Preview 性能・設定](preview-performance.md)
- [1.30 互換性](upstream-1.30.md)
- [GPU と起動のトラブルシューティング](troubleshooting.md)
