# 調査: shared=ref の実体共有と再bind

状態: **未実装・調査PR**。基準は `main@7ed41e0` と固定 MikuMikuDayo130。

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

今回行ったのはソース比較。実行時の共有はこのPRでは実装していない。
