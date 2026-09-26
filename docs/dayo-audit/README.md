# Dayo再利用・互換性調査（2026-09-26）

基準は `main@7ed41e0` と [MikuMikuDayo130 release](https://github.com/pennennennennennenem/MikuMikuDayo/releases/tag/MikuMikuDayo130)。
`.cache/mikumikudayo/MikuMikuDayo130.zip` のSHA256はlockの
`7d5b9bc0183ea24f9fce24a91739346d50dace7ef022d75cd6bf265e12d0a6bb`と一致し、
展開済み `MikuMikuDayo/src` の16ファイルもZIP内容とbyte単位で一致した。
1.20 ZIPも比較対象としたが、修正の基準はlockどおり1.30。

## 確認した差分とPR

| 項目 | 状態 | PR / 根拠 |
| --- | --- | --- |
| buffers[].filenameの欠落、初期uploadなし | 修正PR | [#245](https://github.com/haturatu/mikumikudesu/pull/245) |
| 外部画像のsize/format優先順位とdispatch寸法不一致 | 修正PR | [#246](https://github.com/haturatu/mikumikudesu/pull/246) |
| 環境光prefilterの内蔵コピーが上流更新に追従しない | 修正PR | [prefilter-reuse.md](prefilter-reuse.md)（このPR） |
| shared=refを別allocationにしており実体共有しない | 未実装・調査Draft | [#247](https://github.com/haturatu/mikumikudesu/pull/247) |
| 標準skinningが独自HLSLで上流helperを再利用しない | 未実装・調査Draft | [#248](https://github.com/haturatu/mikumikudesu/pull/248) |
| FX Debugのtexture preview / buffer・texture dumpが未接続 | 未実装・調査Draft | [#249](https://github.com/haturatu/mikumikudesu/pull/249) |
| outputFileと末尾番号を画像連番出力へ適用しない | 未実装・調査Draft | [#250](https://github.com/haturatu/mikumikudesu/pull/250) |
| size.baseの前方参照、相対サイズの最小1の差 | 未実装・調査Draft | [#251](https://github.com/haturatu/mikumikudesu/pull/251) |

調査Draftには実装差分を含めず、上流/現行の関数・影響・接続設計・受け入れ条件を記録した。
各PRはmainから独立している。表の修正はPR段階であり、mainへmerge済みという意味ではない。

## 調査対象の対応関係

| 本家の領域 | desu側の主な経路 | 再利用/移植の境界 |
| --- | --- | --- |
| YRZFx.ixx、Expr.ixx | core/effect、core/fx、fx、NativeFxRuntime | 定義/式はC++移植、同梱FX/HLSLはDXCでcompile |
| YRZ.ixx、renDayo.h | graphics/native_*、Subayai/BDPT、Vulkan | D3D12 hostはVulkanへ置換、PDF/SHは上流HLSLを実行 |
| PMXLoader.ixx、hokanDayo.h、expDayo.h | external/libmmd、core/solver、animation、motion | データ/solver移植。Windows数値oracleは未実施 |
| defsDayo.h、saveDayo.h、keyframeDayo.h | core/project、vmdayo、editor | project/track DTOと保存形式を移植 |
| matDayo.h | core/fx/fx_material、FxMaterialSceneRuntime | template/annotationをlink、GPU tableとdescriptorへ接続 |
| debDayo.h | core/fx_debug、NativeRendererCoordinator、app | metadata/live allocation表示まで |
| dayo.cpp、gizmoDayo.h、YRZImGui.ixx | app/application、core/editor、ImGui/SDL | UI/操作を別hostへ移植 |
| Wave.ixx | core/media、audio/video export、FFmpeg | platform依存のI/Oを置換 |

Windows/DirectX依存C++を直接リンクしていないこと自体を不具合とは扱わない。
再利用可能なHLSLがコピー/独自実装になっている点と、読み込んだ宣言が実行経路まで届かない点を抽出した。

## 対応済みと未検証の区別

- CloneCountはnative scene tableから実際にupload/bindされる。文書の旧い留保だけを根拠に未bindとしない。
- MatDescのrenderer-local/deformer/shared-source texture解決は実装済み。ただしFXのshared=refとは別。
- skyboxPDF/skyboxSH、globalVarSize buffer、controller、typed pass executorにも既存接続がある。
- 全FXのparser/linker/shader compile成功は全FXの実行結果の一致を保証しない。
- HDR/float DDSや全DXGI format保持、Windowsでの.dayo/.vmdayo往復、制限IK・camera/parentの数値比較、
  RT対応GPUでの画像比較は既存の未検証/制限事項として残る。今回、新たな対応済み扱いにはしない。

今回の調査は固定sourceと呼出経路の比較、および修正範囲のCPU/Mock/compileテスト。
全UI操作・全公式エフェクトの実機動作を網羅した認証ではない。
