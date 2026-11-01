# shared=ref の実体共有と再bind

FX の `shared=ref` は、同じ名前の GPU リソースをもう一つ作るという意味ではありません。元と同じ実体を共有し、reload 後の参照も正しく更新する必要があります。

状態: **実装済み**。基準は `main@7ed41e0` と固定 MikuMikuDayo130。

## 調査時点の差分

二つのリソースを個別に確保してしまうと、見た目は似ていても上流の参照関係とは異なります。

上流の `createTextureLambda` / `createTexture3DLambda` / `createBufferLambda` は、`shared=ref` に対して `FindSharedSource(name)` を使います。COM の実体を共有し、source が解放されても参照先の寿命を保持します。

reload 後の参照は `FXWatcher` が更新します。つまり、**共有するのは名前ではなくリソースの実体とその寿命**です。

[FxResourceRuntime::initialize](../../src/graphics/fx_resource_runtime.cpp) は調査時点では 3 種とも `shared` を見ず
`createTextureEx` / `createBufferEx` を呼ぶ。同名sourceが存在しても新規の別GPUリソースになり、
shader compilation や descriptor 生成が成功しても、source の出力を読み取れない状態でした。

[FxSharedResourceRegistry](../../src/graphics/fx_shared_resource_registry.cpp) はsourceを公開できるが、
[NativeRendererCoordinator](../../src/graphics/native_renderer.cpp) のconsumerはMatDesc texture resolver。
これはFX宣言のref allocationではない。publishはgeneric effect実行後なので、初期化時にはまだ
sourceがない場合がある。registry自体もnon-owningで、そのhandleをborrowするだけではreload時に失効する。

## 対応方針

1. 宣言からsource/ref依存を集め、resource確保とpass/pipeline構築を段階化する。
2. 2D/3D/bufferの型・stride・usageを検証し、refをstoreのborrowed resourceとして登録する。
3. owner/generationとGPU完了時点までの寿命を保持し、source削除/reload/resize時にdescriptorを再bindする。
4. producer→consumerのstage順とbarrierを作る。MatDescだけでなく全passのread/writeを対象とする。
5. 未解決・型不一致・名前重複・循環を診断する。本家のdummy fallbackとdesuのambiguous拒否の差を明記する。

## 受け入れ条件

- 異なるFX間で2D/3D/bufferが同じ実体を参照し、consumer側は重複確保/二重解放しない。
- producerの書込値をconsumerからreadbackできる。
- sourceのresize、reload、削除と複数frame in flightで古いhandleにアクセスしない。
- postprocess、renderer、deformerをまたぐ依存を検証する。

## 実装内容

2D / 3D / buffer の参照を初期化前に解決し、source と同じ物理 handle と所有権 token を store に登録します。次元、buffer の stride / type、必要な usage も照合します。

共有先ではリソースをもう一度確保せず、最終所有者が GPU 完了を待って解放します。refresh 時に source の handle / token が変わっていれば descriptor と pipeline を再構築します。

generic stage内はsource→refの依存順に実行し、effect間にmemory barrierを置く。
deform→renderer→postprocessの境界を越えて参照可能で、後続stageへの逆向き参照は診断する。
全stageのsource名重複、未解決、stage内循環も診断する。上流のdummy fallbackは採用せず明示エラーとする。

sourceは実行後に公開されるため、consumerはproducerの書込みと公開後に初期化される。
この実装はsourceのサイズがrefに依存する循環構成をサポートしない。

effect stack変更時に全consumerの宣言・pass用途を収集し、sourceの実formatに対するusageを集約する。
renderer/generic sourceの確保にunionを適用し、consumer追加でusageが増えた既存sourceはrefreshで再確保する。
2D/3D UAV→SRV、SRV→UAV、および2D UAV→RTV、RTV→SRV、depth→sampledのfixtureで
usage・handle共有・追加consumerによる再確保判定を確認する。
waitIdle例外でもtexture/bufferを独立して解放し、device lossを模擬するfixtureも追加した。
実GPU/Windowsとの比較は未実施。前提PR: #253。
