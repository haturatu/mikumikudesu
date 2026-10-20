# #310: フレーム周辺処理の計測と採用判断

基準は main `8a37282`。GCC Release、Vega 8 / RADV、1280×720ウィンドウと既定ImGuiレイアウト、
`--hidden --no-validation --renderer preview --present immediate --preview-quality apu`。
Teto（31,954頂点）＋telephone_merged.vmdを各個体に読み込み、1・3・5モデル、各300フレームを3回計測した。
起動・ロードはフレームプロファイルの外。各条件の前に60フレームでキャッシュを温めた。

```sh
python scripts/benchmark-preview.py --binary build/linux-release/mikumikudesu \
  --model assets/models/teto/model/teto.pmx --motion telephone_merged.vmd \
  --output /tmp/preview-bench --models 1 3 5 --runs 3
```

| モデル数 | animation前/後 ms | render前/後 ms | GPU前/後 ms | フレームp95前/後 ms |
| --- | --- | --- | --- | --- |
| 1 | 2.26 / 2.32 | 0.42 / 0.42 | 6.21 / 6.40 | 12.80 / 14.10 |
| 3 | 10.88 / 10.81 | 0.46 / 0.46 | 16.35 / 16.35 | 39.67 / 39.64 |
| 5 | 33.47 / 28.20 | 0.48 / 0.47 | 22.02 / 22.04 | 100.76 / 87.76 |

各値は3回の中央値。5モデルのp95範囲は前85.35–101.10ms、後81.56–97.18msで重なっている。
GPUクロック・他プロセス・進行したVMD時刻・適応物理ステップ数は固定していない。
この比較から小粒な変更によるFPS向上を主張しない。CPU区間とGPU区間は重なるため加算しない。
GPU時間には取得遅延があり、p95は実際のループ周期（直近4096サンプルまで）を別に集計する。

## 判断

- submitのwait列は採用。最大3待機を固定配列とcountで表現し、semaphore/value/stageを同時に追加する。
  3本のvectorの確保・拡張をなくすことはコード上で確定し、同期順序・stage maskは維持する。
  採用根拠はこの確保削減と上限の明示であり、上表の時間差ではない。
- 重力設定の共通化は採用。同じanimationFrameに対する検索を1回へ減らし、剛体とsoft bodyで共有する。
  モデルごとの呼び出しは同じ値を返すため、物理の刻み・反復回数・順序を変えない。
- materials/drawsのPMR reserveは不採用。4MiBのscratch内の確保とヒープ確保を区別できる計測が先に必要。
  材質の冗長再構築は#282の範囲。
- 実行計画の単純な再利用は不採用。評価後のanimationVisibleを保持する必要がある。
  変更のない部分と表示状態の分離は別の変更として検証する。
- AO深度と本パスの共有は不採用。この計測だけでは互換性・透明面・ジッター・同期を変える根拠がない。
- shadowのキャッシュは#282、FXは#279、画像出力readbackは#281、BLAS/TLASは#278で扱う。

GPUパス別時間をCLIでも出力し、タイムスタンプの取得できたサンプルだけで平均する。
基準の1モデル・1回目はshadow 0.428、depth/AO 1.067、materials 1.221、SSS 0.236、
Bloom 0.524、tonemap 1.578ms。DoFは停止していた。
これらは画像内の各区間であり、GPU合計との差には頂点変形などの区間も含まれる。

検証: coreの集計・p95・resetテスト、通常/High/非同期/1080p輪郭のGPUテスト。
スクリーン出力の既存比較とVulkan同期validationを維持する。
