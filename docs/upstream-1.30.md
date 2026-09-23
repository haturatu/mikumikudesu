# MikuMikuDayo 1.30互換性

## この更新の範囲

公式[1.30 Release](https://github.com/pennennennennennenem/MikuMikuDayo/releases/tag/MikuMikuDayo130)を
テスト・配布の基準にする第1段階です。Release APIで確認したasset digestをlockに固定し、取得したZIPのSHA256をlockと照合します。
`deps/mikumikudayo.lock`が取得元の正本で、アプリ自身のバージョンとは独立しています。

`deps/mikumikudayo-runtime.manifest`はrelease rootからの相対パスを1行ずつ記述します。
末尾`/`はディレクトリ全体、それ以外は単一ファイルです。空行と`#`コメントを許可します。
fetchは全エントリと主要renderer・画像・ライセンスの存在を検証し、成功した場合だけ置換します。
installも同じ一覧を使い、拡張子による除外を行わず、第三者ライセンスとtexture/importを保持します。

## 対応段階

| 段階 | 完了条件 | 状態 |
| --- | --- | --- |
| 1. baseline・配布 | 1.30 ZIPの固定、SHA256検証、共通manifestによる配布、公式assetの既存互換テスト | 実装済み |
| 2. データ・solver互換 | Windows 1.30保存fixtureの往復、camera/external parent/制限IKの数値比較 | データ形式は実装済み、実機検証は未実施 |
| 3. FX 1.30契約 | buffer/size expression、pow、CloneCount/CLONEDVERTEXCOUNT、MatDesc、resource allocation、RT hit groupの接続 | IR/parser/planner、MatDesc scene table/per-pass descriptor path、host screen.bmp・deformer texture参照、live FXのshared=source registry、renderer-local texture参照、typed executorは実装済み。shared=ref allocationと上流全FXのruntime検証は未完了 |
| 4. Subayai/BDPT実行 | Vulkan BLAS/TLAS/SBT、各pass実行器、native frame/output bridge、RT対応GPUでの画像比較 | runtime接続・feature fallback・CPU/Mock検証は実装済み、RT対応GPUでの画像比較は未実施 |
| 5. FX Debug | resource/pass/controllerの実行時inspect、texture preview/dump | graph metadataとnative GPU allocationの実体サイズ・形式のinspectは実装済み。readback preview/dumpは未完了 |

`nativeSubayai`/`nativeBdpt`は起動時のGPU capability、選択したFX graphの要求feature、native runtimeの
初期化結果をすべて満たした場合だけ有効になります。Previewのclone複製は、上流FXが参照する
`Dayo::CloneCount[modelIndex]`との接続完了を意味しません。1.30の追加エフェクトを同梱しても、
その実行をサポートしたことにはなりません。OIDNは任意検出で、2.5.0への固定は行いません。
非同期画像出力はbounded queueまで対応済みです。ファイル名末尾からの連番開始は別の残件です。

## Dayo environment host resources

固定した1.30 sourceの`resources.hlsli`/`yrztypes.hlsli`に合わせ、`Dayo::Skybox`は線形の2:1
`Texture2D<float4>`、`Skywalker`/`SkywalkerRow`は12-byte Walker要素、`SkyboxSH`は9個の`float4`
としてscene descriptorへ渡します。環境分布とSHはC++へ移植せず、同梱`hlsl/system/skyboxPDF.hlsl`と
`skyboxSH.hlsl`のentry pointをDXCでSPIR-V化してGPU上で実行します。テストではABI stride、pass順、各entry
pointのdispatch group、memoによるpass選択を検証します。Windows 1.30とのGPU数値比較はまだ行っていません。

`SkyboxPrefilter` memoも、上流の2D equirectangular texture向け`PrefilterEnvmap`をGPUで実行します。
元画像をmip 0へコピーし、上流と同じroughness・alpha blend・サンプル数・4 iterationで各mipを生成します。
Subayai用cubemap prefilterとは別経路です。テストはdispatch計画とmip別render targetを検証しますが、
Windows 1.30とのGPU数値比較はまだ行っていません。未知memoは保持しますが、既知handlerがないものは未対応です。
`globalVarSize`は固定1.30 sourceの既定1024 byteを使い、FX instanceごとに永続uniform bufferを生成します。
DXCの`-fvk-bind-globals 50 0`で暗黙の`$Globals`をset 0 / `b2`へ固定し、CPU側のbindingと一致させます。
glslcには同等の指定がないためnative YRZFX shaderでは拒否し、他用途のHLSL compile fallbackにだけ使います。
上流sourceにもglobal CBへのCPU writeが無く、desu側もゼロ初期化までです。更新データの意味論・Windows GPU結果は未検証です。

FX external DDSはBC1–BC5および32-bit RGBA/BGRAをRGBA8へdecodeし、DX10/legacyの2D image、3D volume、cubemapと各mipを読み取ります。
`textures3D`のexternal DDSは3D textureとしてallocateし、ファイル内のmipを個別uploadします。現在のbackend
upload ABIに合わせたRGBA8 decodeであり、DXGI format保持、全DXGI形式、FXからのcubemap宣言は未対応です。

`.dayo`の`EditorInfo`は1.30 sourceで定義された全フィールドをproject DTOと両方のJSON sectionへ保持します。
repeat、audio volume/offset、floor collision、録画範囲など、desuに対応する設定はruntimeへ適用します。
その他のupstream editor settingはload/saveで保持しますが、同じUIや描画動作を実装したことを意味しません。

MatDescのordered value payloadは、生成HLSLのfield宣言順とDXCのDirectX buffer row packingに合わせるCPU packerを持ちます
([DXC SPIR-V layout rules](https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/SPIR-V.rst),
[DXC buffer packing](https://github.com/microsoft/DirectXShaderCompiler/wiki/Buffer-Packing))。
CPU側ではモデル→material base index、明示logical slotから2D/3D別physical texture indexへの変換、texture dedup tableまで構築します。
生成HLSLは未割当slotでdescriptor index 0へfallbackし、`hasTexture`をfalseにします。generic `_idx`/`_tex`/`_tex3D`/`_value`
registerはpass binding planから生成し、Texture3D descriptorは独立したsecondary setの`t0`へ置きます。
`FxMaterialGpuRuntime`はfallbackをdescriptor index 0へ予約し、dedup済み2D/3D textureとframe-slot別の4つのstructured bufferを生成・更新します。
`NativeFxRuntime`/`DayoFxRuntime`はこれらをper-pass descriptor setへ結び、2D/3D descriptor array長とsecondary setを構築し、buffer再確保時にdescriptorを更新できます。
`FxMaterialSceneRuntime`はモデル別material annotation/defaultFileをlinkし、各frameで式を評価してscene-wide GPU tableへ反映します。
Subayai/BDPT generic FXへこのtableを渡し、annotation textureのうち予約された`screen.bmp`、active renderer-local texture、owner modelに割り当てられたdeformer texture、
およびlive FX runtimeが`shared=source`として公開したtextureは、resource ownerを維持したままgeneration付きexternal descriptorとして参照します。
共有source名が複数runtimeに存在する場合は誤選択せず未解決として扱います。deformer resource名のうち未解決かつfile-likeでないtokenはdimension別fallbackへ解決します。
画像は現在RGBA8へdecodeし、DDS 2D/3Dと通常画像のmipmapを扱います。この実装はlive runtimeのtexture source解決に限定され、
FX宣言の`shared=ref` resource allocation/aliasing、実行前のpostprocess source解決、完全な上流互換、Windows 1.30とのGPU画像比較は未完了です。
2D/3D texture objectを含むMatDesc getterの生成HLSLは、
aggregate zero-initializationを避けるfield-wise loweringを行い、compile fixtureでSPIR-V生成まで確認します。

## Windows fixtureの受け入れ条件

Windows 1.30実行環境での保存・再読込は今回のLinux検証に含みません。
独自serializerで生成したファイルを「本家が生成したfixture」として扱わないでください。
追加するfixtureには、生成に使ったRelease、操作手順、モデルの出典・再配布条件を記録します。

- `camera-parent.vmdayo`: parentModel/parentBone/parentBoneNameの一致・欠落・再解決
- `catmull-axis.vmdayo`: boneの4軸、cameraの6軸それぞれの補間method
- `multi-model.dayo`: camera/light subsetと複数モデルsubset、dictionary、metadata、全track
- `external-parent.dayo`: 親モデル削除、未解決親、循環参照
- `gravity.dayo`: 重力keyと保存設定

本家save → native load → native serialize → native reloadで上記項目と未知payloadの保持を検証し、
続いてnative save → 本家loadを確認します。制限IKは1/2/3軸、膝型・非対称limitを持つ小さいPMXを
同じframeで評価し、bone matrixとendpointの誤差、許容値、実行環境を記録します。

## 再現コマンド

```bash
python3 scripts/fetch-mikumikudayo.py
cmake --preset linux-debug -DDAYO_WARNINGS_AS_ERRORS=ON
cmake --build --preset linux-debug
ctest --preset linux-debug --output-on-failure -R '^(core|upstream_fetch|upstream_compat)$'
cmake --install build/linux-debug --prefix /tmp/mikumikudesu-130-package
```

`upstream_fetch`はネットワーク不要の合成ZIPによるinstaller/packagingの回帰テストです。
`upstream_compat`は取得済み公式assetによるgraph展開、画像読込、VMD/PMXの互換テストです。
Windows実機の相互運用結果とは区別します。
