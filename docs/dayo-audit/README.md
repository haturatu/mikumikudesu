# MikuMikuDayo 1.30 の再利用・互換性調査

この文書は、固定した上流ソースと mikumikudesu の実装を比較し、調査対象と修正履歴をまとめたものです。表の「実装済み」は該当するコード経路の対応状況を示します。すべての FX、GPU、モデルについて本家との結果一致を保証するものではありません。

## 調査の前提

比較の基準は `main@7ed41e0` と [MikuMikuDayo130 release](https://github.com/pennennennennennenem/MikuMikuDayo/releases/tag/MikuMikuDayo130)。
`.cache/mikumikudayo/MikuMikuDayo130.zip` のSHA256はlockの
`7d5b9bc0183ea24f9fce24a91739346d50dace7ef022d75cd6bf265e12d0a6bb`と一致し、
展開済み `MikuMikuDayo/src` の16ファイルもZIP内容とbyte単位で一致した。
1.20 ZIPも比較対象としたが、修正の基準はlockどおり1.30。

## 対応した項目と関連 PR

| 項目 | 状態 | PR / 根拠 |
| --- | --- | --- |
| buffers[].filenameの欠落、初期uploadなし | 実装済み | [#245](https://github.com/haturatu/mikumikudesu/pull/245) |
| 外部画像のsize/format優先順位とdispatch寸法不一致 | 実装済み | [#246](https://github.com/haturatu/mikumikudesu/pull/246) |
| 環境光prefilterの内蔵コピーが上流更新に追従しない | 実装済み | [#252](https://github.com/haturatu/mikumikudesu/pull/252)、[詳細](prefilter-reuse.md) |
| shared=refを別allocationにしており実体共有しない | 実装済み | [#247](https://github.com/haturatu/mikumikudesu/pull/247) |
| 標準skinningが独自HLSLで上流helperを再利用しない | 実装済み | [#248](https://github.com/haturatu/mikumikudesu/pull/248) |
| FX Debugのtexture preview / buffer・texture dumpが未接続 | 実装済み | [#249](https://github.com/haturatu/mikumikudesu/pull/249) |
| outputFileと末尾番号を画像連番出力へ適用しない | 実装済み | [#250](https://github.com/haturatu/mikumikudesu/pull/250) |
| size.baseの前方参照、相対サイズの最小1の差 | 実装済み | [#251](https://github.com/haturatu/mikumikudesu/pull/251) |
| generic deformer/postprocessへMatDesc GPU tableを渡さない | 実装済み | [#253](https://github.com/haturatu/mikumikudesu/pull/253) |
| texture.typeを保持せず、外部画像のtyped formatも限定 | 実装済み | [#254](https://github.com/haturatu/mikumikudesu/pull/254) |

PR の open / merged 状態と検証結果は各リンク先で確認してください。この一覧は調査・修正時点の記録です。
実装時のstackは `#246 → #251 → #254 → #253 → #247 → #248 → #249 → #250`。
prefilter再利用の[#252](https://github.com/haturatu/mikumikudesu/pull/252)はmainをbaseとする独立PR。

## 上流コードと移植先の対応

| 本家の領域 | desu側の主な経路 | 再利用/移植の境界 |
| --- | --- | --- |
| YRZFx.ixx、Expr.ixx | core/effect、core/fx、fx、NativeFxRuntime | 定義/式はC++移植、同梱FX/HLSLはDXCでcompile |
| YRZ.ixx、renDayo.h | graphics/native_*、Subayai/BDPT、Vulkan | D3D12 hostはVulkanへ置換、PDF/SHは上流HLSLを実行 |
| PMXLoader.ixx、hokanDayo.h、expDayo.h | external/libmmd、core/solver、animation、motion | データ/solver移植。Windows数値oracleは未実施 |
| defsDayo.h、saveDayo.h、keyframeDayo.h | core/project、vmdayo、editor | project/track DTOと保存形式を移植 |
| matDayo.h | core/fx/fx_material、FxMaterialSceneRuntime | template/annotationをlink、GPU tableとdescriptorへ接続 |
| debDayo.h | core/fx_debug、NativeRendererCoordinator、app | metadata/live allocation、texture previewとdump |
| dayo.cpp、gizmoDayo.h、YRZImGui.ixx | app/application、core/editor、ImGui/SDL | UI/操作を別hostへ移植 |
| Wave.ixx | core/media、audio/video export、FFmpeg | platform依存のI/Oを置換 |

Windows / DirectX 依存の C++ を直接リンクしていないこと自体は不具合とは扱いません。
再利用可能なHLSLがコピー/独自実装になっている点と、読み込んだ宣言が実行経路まで届かない点を抽出した。

## 実装済みの機能と未検証事項

- CloneCountはnative scene tableから実際にupload/bindされる。文書の旧い留保だけを根拠に未bindとしない。
- MatDescのrenderer-local/deformer/shared-source texture解決は実装済み。ただしFXのshared=refとは別。
- skyboxPDF/skyboxSH、globalVarSize buffer、controller、typed pass executorにも既存接続がある。
- 全FXのparser/linker/shader compile成功は全FXの実行結果の一致を保証しない。
- HDR/float DDSや全DXGI format保持、Windowsでの.dayo/.vmdayo往復、制限IK・camera/parentの数値比較、
  RT対応GPUでの画像比較は既存の未検証/制限事項として残る。今回、新たな対応済み扱いにはしない。

この調査では、固定した上流ソース、呼び出し経路、修正対象の CPU / Mock / compile テストを確認しました。全 UI 操作・全公式エフェクトについて実機での一致を確認したものではありません。
