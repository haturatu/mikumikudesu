# 調査: shared=ref の実体共有と再bind

状態: **実装済み**。基準は `main@7ed41e0` と固定 MikuMikuDayo130。

## 確認した差分

本家 `src/YRZFx.ixx` の `createTextureLambda` / `createTexture3DLambda` / `createBufferLambda`
は `shared=ref` で `FindSharedSource(name)` を使う。参照元のComポインタを共有し、source解放時にも
参照先の実体を保持する。`FXWatcher` の共有参照更新処理がreload後の参照を更新する。

[FxResourceRuntime::initialize](../../src/graphics/fx_resource_runtime.cpp) は3種とも `shared` を見ず
`createTextureEx` / `createBufferEx` を呼ぶ。同名sourceが存在しても新規の別GPUリソースになり、
shader compilationやdescriptor生成が成功してもsourceの出力を読めない。

[FxSharedResourceRegistry](../../src/graphics/fx_shared_resource_registry.cpp) はsourceを公開できるが、
[NativeRendererCoordinator](../../src/graphics/native_renderer.cpp) のconsumerはMatDesc texture resolver。
これはFX宣言のref allocationではない。publishはgeneric effect実行後なので、初期化時にはまだ
sourceがない場合がある。registry自体もnon-owningで、そのhandleをborrowするだけではreload時に失効する。

## 対応案

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

## 実装

2D/3D/buffer の ref を初期化前に解決し、source と同じ物理handleと所有権tokenをstoreへ登録する。
sourceの次元・buffer stride/type・必要usageを検査する。共有参照は重複確保せず、最終所有者がGPU待機後に解放する。
refresh はsourceのhandle/token変更を検知してdescriptorとpipelineを再構築する。

generic stage内はsource→refの依存順に実行し、effect間にmemory barrierを置く。
deform→renderer→postprocessの境界を越えて参照可能で、後続stageへの逆向き参照は診断する。
全stageのsource名重複、未解決、stage内循環も診断する。上流のdummy fallbackは採用せず明示エラーとする。

sourceは実行後に公開されるため、consumerはproducerの書込みと公開後に初期化される。
この実装はsourceのサイズがrefに依存する循環構成をサポートしない。

Linux `mikumikudesu` ビルド成功。今回テスト・GPU readbackは行っていない。前提PR: #253。
