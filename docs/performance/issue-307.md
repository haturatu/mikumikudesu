# #307: モーフ依存グラフの事前計算

libmmdの`MorphWeightExpander`をMmdAnimator::Implでモデルの寿命にわたって保持する。
indegree/dependents/queueによる循環判定と、循環を除く伝播順は構築時に計算する。
フレームごとにはpropagated/effectiveWeightsの値を初期化し、保存した順序で伝播する。
出力もImpl内に保持し、applyMorph中だけ参照する。返却フレームは独立した所有データのまま。

単純なトポロジカルソートへの置換ではない。元実装のLIFO順、循環グループの直接leaf評価、
無効参照・非有限値の除外、無効offsetも含むmaxStepsの消費順を保持する。
空/ゼロ入力でも前フレームのウェイトやbudgetExceededを残さない。
単発利用のexpandMorphWeightsはAPIを維持し、その場でexpanderを構築する。

## 検証と測定

libmmd Debug有効14テスト成功。反復入力、ゼロ、NaN、循環、予算切替、出力の領域維持、入力/出力aliasを検証。
変更前`ff0e720`の関数を独立した比較対象としてビルドし、seed 307の1,000グラフ×10入力×6予算の
60,000ケースでeffectiveWeights/cycleDetected/budgetExceededが完全一致した。

`morph_expansion_benchmark.cpp`は4,096ノードの鎖を300評価×3回ずつ交互に処理する単体測定。
`animation_benchmark.cpp`と同じReleaseライブラリへのリンク方法でビルドできる。
単発API（毎回構築）の平均中央値0.2325ms、保持済みexpanderは0.0285ms。各checksumは一致。
これは意図的に大きなグラフの展開だけを測った値で、通常再生のFPSや全アニメーション時間ではない。
MmdAnimatorは保持済みの経路を使う。モーション差替えでモデルの依存グラフは変わらない。
