# 調査: deformer/postprocessのMatDesc GPU binding

状態: **実装済み**。基準は `main@7ed41e0` と固定 MikuMikuDayo130。

本家 `src/YRZFx.ixx` は `FXTech::matDescs` をcategory共通で保持し、`FX::Load` の
`hasMatDesc()` 分岐でtemplateを読み、各passへidx/tex/tex3D/value/texture配列を生成する。

[FxCompiler::compile](../../src/fx/fx_compiler.cpp) はdesuでもcategoryに関わらずmaterialSchemaを作る。
ただし [NativeRendererCoordinator::executeGenericEffects](../../src/graphics/native_renderer.cpp) の
GenericEffectRuntimeはcontroller/global bufferだけを作り、`initializeForFrame` と `refresh` に
material runtime/initializerを渡していない。
[FxPassDescriptorRuntime](../../src/graphics/fx_resource_runtime.cpp) の `@matdesc/` bindingは
material runtimeがnullだと失敗するため、MatDesc付きgeneric deformer/postprocessの実行経路が欠けている。

Subayai/BDPTの `FxMaterialSceneRuntime` 接続は存在する。rendererでmaterial annotationが使えることと、
generic stackでも使えることは別の契約である。実際のGPUエラー再現は未実施で、呼出経路と
null検査から判断した到達可能な初期化失敗である。

対応案はGenericEffectRuntimeにもeffectごとのFxMaterialSceneRuntimeを所有させ、modelごとの
annotation/default file・texture resolver・frame式contextを初期化前に渡すこと。
texture resolverはそのeffectのローカルresourceを優先し、renderer-local/source側を誤って選ばない。
model構成、material override、reload、texture generation、layout長の変化で再linkする。

受け入れ条件は、deformerとpostprocessそれぞれでMatDesc付きfixtureを起動し、
値・2D/3D texture・defaultFile・式更新・初回local texture解決をGPU table/descriptorで確認すること。
複数generic effectで同名field/resourceを使ってもeffect間で混ざらないことも確認する。

## 実装

GenericEffectRuntime が effect ごとに FxMaterialSceneRuntime とコンパイル済み schema を所有する。
初回はリソース確保後の initializer で、以後は毎frameの sync と resize時の initializer で
annotation、defaultFile、式、texture catalogを更新する。配列長変更時はdescriptor/pipelineを再構築する。

共通のtexture resolverを抽出し、generic側は当該effectのlocal storeを最優先にする。
renderer-localへの暗黙参照を避け、screen、owner deformer、明示shared sourceを順に参照する。
モデルごとのscene material indexを保持し、deformer ownerにはeffectのclone countを適用する。

Linux `mikumikudesu` ビルド成功。今回テスト・GPU実行は行っていない。前提PR: #254。
