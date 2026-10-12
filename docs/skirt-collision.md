# スカートと脚の衝突補正（P0 / P1）

Bullet の接触応答を修正し、スキニング後のスカート表面に共通 Compute Pass を追加します。
Preview は補正後のバッファを Shadow / Normal / Main / Edge で共有し、Native は補正後の位置から
BLAS を更新します。既存の剛体・ジョイント・ボーンへの物理反映を維持します。

## P0: キネマティック速度と物理の精度

- サブステップ直前に interpolation transform を上書きしません。Bullet が前の姿勢から並進・角速度を計算します。
- reset / teleport では interpolation transform と速度をリセットし、シークの移動を接触速度にしません。
- 特定した脚のカプセルについて、時間、最短回転角（5度/step）、移動量（半径の0.25/step）の最大値から
  サブステップを選びます。動作による追加は16までとし、従来の固定時間による必要ステップ数は削減しません。
- スカートと脚を特定できたモデルのみ solverIterations を25にします。その他は従来の10です。
- `MmdPhysics::stepStats()` は実際のステップ数、動作予算の超過、PMX剛体IDと接触深度を返します。
  接触数は全サブステップの接触点数で、同じ剛体ペアが繰り返し含まれます。

libmmd の変更: <https://github.com/haturatu/libmmd/pull/15>

## P1: 表面とカプセルの補正

`identifySkirtCollision()` は動的スカート剛体、脚・ひざのキネマティックカプセル、双方の衝突マスクを調べます。
材質名に skirt / スカート / 裙 があれば、その材質を対象にします。名前付き材質がない場合のみ、
スカート剛体に接続するボーンのウェイトから対象材質を推定します。足首は対象から除きます。
YYB式初音ミクv1.02では895頂点、1,192三角形、16スカート剛体、4脚カプセルです。

- カプセルの中心線は剛体のローカルY軸を回転させて作ります。位置、半径、余裕0.03をメッシュと同じ座標系に変換します。
- 初期化で頂点を押し出し、通常の接触拘束と近傍の補正量の平滑化を最大6反復行います。
  前フレームの補正済み位置で、深い貫通時の外向き方向を選びます。
- 三角形とカプセル中心線の最短距離を検査します。端点・辺・面内部の交差も検出するため、
  頂点や重心が外側でも三角形が脚を横切るケースを検出できます。
- 残る実際の貫通は、胴体の向きと脚の軸から求める共通の支持平面で解消します。
  回復中の平滑化は衝突拘束を打ち消すため行いません。各頂点が自分の結果だけを書き、反復間は別のバッファを読み書きします。
- 腰に従う頂点の移動率は0.25、スカートの物理ボーンに従う頂点は最大1です。
  総補正量は最大脚半径の6倍までに制限し、残る貫通を診断に記録します。
- 最後に面積で重み付けした法線を更新します。材質の元の法線方向を維持し、対象外の頂点は変更しません。
- 初回、シーク、逆再生、繰り返しフレーム、モデル構成の変更では履歴を初期化します。

`SkirtCapsule` の前フレームの端点は `previousA/B.xyz` です。未使用だった2つの `w` は、回復用法線の
octahedral encodingに使います。現在の端点の `aRadius.w` と `bMargin.w` は半径と余裕です。

フレームごとの入力と作業領域はframe slotごとに持ち、履歴だけを共有します。
スキニング→補正、反復間、前フレームの履歴→次フレームにメモリ依存を置きます。
Previewのasync computeでも描画が補正後の出力を待ちます。Nativeの通常フレームは
`record(commands, upload, generation)` で補正を行い、その後BLAS更新の依存を置きます。
引数なしの旧 `record(commands)` はスキニングのみの互換経路です。

## 品質と診断

既定は32反復です。Preview / Native / 動画書き出しで同じ値を使用します。

```bash
./build/linux-debug/mikumikudesu --skirt-collision-iterations 32 --asset "$PMX" --asset "$VMD"
./build/linux-debug/mikumikudesu --skirt-collision-iterations 16 --asset "$PMX" --asset "$VMD"
./build/linux-debug/mikumikudesu --skirt-collision-iterations 0 --asset "$PMX" --asset "$VMD"
```

範囲は0〜64で、0は表面補正を無効にします。少ない反復は処理時間を減らしますが、急な足上げやシークで
貫通が残る場合があります。今回のYYB / UNDEADの合格値は既定の32です。
`SkirtCollisionUpload::iterations = 0` はテスト用の診断のみの経路で、CLIの無効化とは区別します。

`skirtRigidStats()` と、GPU完了後の `SkirtCollisionRuntime::readStats()` / Previewの診断APIで、
剛体の接触数と深度、貫通頂点・三角形数、メッシュの最大深度、総補正距離を取得できます。
通常描画では診断のreadbackによる待機を入れません。メッシュの判定深度には0.03の余裕を含みます。

## 回帰テスト

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --test-dir build/linux-debug --output-on-failure

PMX='/path/to/yyb-miku.pmx'
VMD='/path/to/UNDEAD.vmd'
VK_LAYER_VALIDATE_SYNC=1 ./build/linux-debug/dayo_skirt_collision_tests \
  "$PMX" "$VMD" /tmp/yyb-skirt.csv
```

DXCと取得済みMikuMikuDayoヘッダーがあればNativeのスキニングも検証します。
アセットはリポジトリに含めません。GPUのないCIでは既存のVulkanテストと同様に実行環境を用意します。

- 合成データ: 双方向のマスク、材質の範囲、回転と正規化、頂点・三角形のみの貫通、
  履歴の側、シーク、退化形状、Preview / Nativeのバッファ配置、GPUスキニングから描画出力までの接続。
- 実アセット: 0〜348を1/30秒で評価し、独立した物理ワールドで同じ結果になることを確認します。
  その後0→127をシークします。全フレームでPreview / Nativeの配置による補正済み位置を比較します。
- 0、115、120、127、130、140、150、165、348とシークで、CPU / Preview GPU / Native GPUの
  実際のスキニングと補正を比較します。CPU/GPUの許容差はPMX座標で0.002、同一入力の補正パスは0.00001です。
- CSVに毎フレームの剛体・メッシュの診断とステップ数を記録します。深度が0.05を超えた場合は
  `.mesh.csv` / `.capsules.csv` に詳細を保存します。`.triangles.csv` は頂点ID、`.timing.csv` はGPU時間です。
- 調査用に `DAYO_SKIRT_FIRST_FRAME` / `DAYO_SKIRT_LAST_FRAME` / `DAYO_SKIRT_ITERATIONS` を指定できます。
  合格判定には指定なしの全フレームを使います。

## 実測結果

YYB式初音ミクv1.02 + 指定UNDEAD、Bullet、Vulkan validation / synchronization validation、既定32反復での結果です。
補正前は、このPRの物理設定でスキニングした直後のメッシュです。

| 条件 | 補正前の最大深度 | 補正後の最大判定深度 | 補正後の貫通頂点数 |
| --- | ---: | ---: | ---: |
| 通常再生0〜348 | 1.03 | 0.0210718 | 0 |
| 右足115〜140 | 1.03 | 0.0210718 | 0 |
| 左足150〜165 | 0.109141 | 0.000000119 | 0 |
| 0→127シーク | 0.863701 | 0.0201478 | 0 |

余裕0.03を含めても上記の深度なので、計測したメッシュと脚カプセル間に実際の貫通はありません。
小さな三角形接触の診断は余裕領域への接触です。通常再生の物理ステップ数は最大11でした。
全フレームでNaN・無限大はなく、Preview/Nativeの配置による位置比較が合格しました。
重点フレームでのCPU/GPUスキニングの比較も合格しました。

AMD Radeon Vega 8 / RADV、Debug、validation有効、フレーム127の30回の測定（最初の6回を除く中央値）では、
同じ描画処理に補正を足したGPU時間は1.855ms→14.909ms、増分13.053msでした。
これはフレーム127の追加Compute処理を含む測定で、通常再生全体の平均FPSではありません。
CPU負荷・GPUクロック・validation・ハードウェアによって変動します。

有効なCTest 23件はすべて合格しました。RadeonでのPreviewとllvmpipeでのNative（実際のBLAS更新を含む）で、
127フレームのMP4書き出しも成功しました。Vulkan同期検証にエラーはありませんでした。

## 適用範囲

この処理は剛体ボーンのアニメーションを維持する表面補正です。脚が激しく入り込む場合には布が伸びることがあります。
カプセルはPMXの剛体形状であり、脚の描画メッシュを直接衝突形状にしているわけではありません。
自動判定に使う名前やウェイトが異なるモデルは対象抽出の追加が必要です。
XPBD、布の伸長・曲げ拘束、自己衝突、物理状態キャッシュは今回のP0/P1には含めません。
