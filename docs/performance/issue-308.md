# #308: トラック探索・作業領域・出力容量の再利用

setMotionで全trackを構築・sortしてからモデルindex別のポインタ列を作る。
差替えとnullptr解除では対応列をクリアし、古いコンテナやsampling cursorを参照しない。
setPoseでは各キーに最初に一致するボーンを対応付ける。重複キーの適用順と同名ボーンの扱いは維持する。

morph、impulseの4方向とreset、physics bones/positionsの作業領域はImplで保持し、各フレームの値を初期化する。
物理後subtree探索のLIFOスタックも再利用する。Bulletの刻み・反復・衝突解決順は変更しない。

evaluateIntoは呼び出し元のフレームを再利用する明示的API。vertices/bones/materials/morphWeights/visibleを毎回初期化する。
既存evaluateは新規フレームを返すため、以前返されたフレームの所有権・寿命は変わらない。
アプリは評価済みフレームのコンテナを保持してevaluateIntoを使い、モデル数変更時に末尾を除去する。
PreviewのmorphWeightsのreserveも全モデルのモーフ数合計に修正した。

## 検証と測定

libmmd Debug有効14テスト、アプリRelease有効31テスト成功。
motion/poseの差替え・解除・再設定、欠落名、同名ボーン、重複ポーズ、visible/morph/material初期化、
出力容量の維持と以前の返却フレームの独立性をテスト。アプリのモデル増減・renderer切替・UV更新も維持する。

`frame_allocation_benchmark.cpp`（1ボーン/1モーフ/1材質、GPU・物理なし、warm後1,000評価）で
operator new呼出しはvalue APIの3,000回に対しevaluateIntoは0回。
これはこの合成条件のC++確保数であり、実アプリ全体のmallocやBulletの確保数を表すものではない。

`animation_benchmark.cpp`の同一バイナリ・Teto+VMD・物理なし・omitモードでは、
value API平均の3回中央値0.0803ms、`--reuse`は0.0812ms。
この計測から速度向上は確認できていない。再利用は確定した確保削減を根拠に採用し、FPS向上は主張しない。
