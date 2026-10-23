# Preview の露出・白色点を事前計算する

HDR トーンマップの各画素／FXAA の近傍参照で行っていた露出の `exp2` と白色点ゲインの計算を、
`setPreviewRenderSettings` へ移した。サニタイズ済み設定から計算して CPU 側に保持し、
既存80-byte push constant の予約領域で渡す。
デフォルトの定数は厳密に identity。`(color * exposure) * whiteBalance` の乗算順序、
view transform、透明部分の bloom 合成、FXAA の処理順序を維持する。
追加の描画パスや画像転送は導入しない。

対象は HDR トーンマップ経路で、前フレーム背景へ直接合成する fallback のカラー計算は従来どおり。

## 検証

main `f10efd8` と比較。Ryzen 5 PRO 3500U / Vega 8、Mesa RADV 26.2.5、glslc 2026.4、GCC Release。
native/PGO/LTO は無効。計測値・ハッシュは [preview-color-constants-results.json](preview-color-constants-results.json)。

- CPU テストで6500 Kの厳密な identity、1000/15000 Kの端点、2222/4000 Kの分岐境界、EV ±12、輝度の正規化を確認。
- 元の fit を double で独立評価した参照値と、白色点ゲインの誤差を相対 `2e-5` 以内で確認。
- 31×19の合成 HDR 画像で、透明度0/0.25/0.5/0.75/1、ACES/AgX/Neutral、白色点7条件、
  EV −12/0/+12、Bloom/FXAA 各on/offを組み合わせた252出力を比較。
  593,712チャンネル中、差分は2チャンネル、最大差は1/255。ビット単位の完全一致ではない。
- Preview 通常／High／async、1080p DoF、effect settings の5テストが成功。
  HDR書き出しとのカラー・Bloom・FXAAの一致、効果切替、キューに残る古い設定も既存テストで検証。

| 対象 | main | 変更後 |
| --- | ---: | ---: |
| SPIR-V bytes | 83,812 | 60,640 |
| SPIR-V 静的命令数 | 4,294 | 3,186 |
| OpFDiv / Exp2 箇所 | 266 / 18 | 86 / 0 |
| RADV GFX9 code bytes | 22,576 | 22,104 |
| RADV 静的命令数 | 3,524 | 3,455 |
| SGPR / VGPR | 48 / 52 | 48 / 48 |
| コンパイラ報告の subgroups/SIMD | 4 | 5 |

最終 ISA でも削減を確認した。両方とも spill / scratch は0、画像 sample は36箇所。
静的命令数は分岐の両側を含み、subgroups/SIMD は実測 occupancy ではない。
この表から GPU 時間／FPSの改善率は算出しない。

```sh
cmake --preset linux-release
cmake --build --preset linux-release --target mikumikudesu dayo_preview_tests dayo_preview_effects_tests -j3
ctest --test-dir build/linux-release \
  -R '^preview_(effect_settings|render(_high|_async)?|dof_edge_1080p)$' --output-on-failure
DAYO_TEST_COLOR_OUTPUT=/tmp/preview-color-new build/linux-release/dayo_preview_tests
```

比較元でも同じ `captureColorReference` テスト関数を使い、製品の実装・シェーダーを元のままで252出力を保存した。
RGBAファイル名はループ順の連番で、全ファイルの全チャンネルを比較した。
ISA は時間計測とは別に `RADV_DEBUG=ps,asm,shaderstats,nocache MESA_SHADER_CACHE_DISABLE=true` で取得。
比較用の生成 SPIR-V の一時差替えは終了時に復元し、両方のハッシュを記録した。
