# #309: 物理結果を変えず接触診断の収集を制御する

PhysicsSettings.collectContactDiagnosticsは既定true。既存のstepStats利用者のcontactsを維持する。
アプリのScene::addModelだけがfalseを指定する。通常実行ではcontactsを参照していない。
無効化するのはBullet stepSimulation後の診断用manifold走査・vectorコピーだけで、衝突検出/解決は継続する。
substepsとmotionBudgetExceededは無効時にも更新する。statsの値は毎回初期化しcontactsの容量だけを保持する。
固定刻み1/120、Solver反復10、適応細分化、品質の既定値は変更しない。

検証: libmmd Debug有効14テスト成功。
接触を伴うカプセルの移動・回転120フレームでON/OFFの全剛体の位置・回転・線形/角速度が完全一致。
ステップ数・予算フラグも一致し、OFFのcontactsは常に空。
既定ON、reset、無効delta、既存の衣装衝突・階層物理テストも成功。

`contact_diagnostics_benchmark.cpp`は256球と床、60ステップwarm後600ステップ×各3回の交互実行。
Release、同じ固定刻みとSolverで、平均の中央値はON 0.3615ms、OFF 0.3584ms。
ONは各回307,200診断接触、OFFは0、最終y座標和は両方76.8。
差は小さく、実モデルのFPS向上を示す値ではない。診断走査とコピーの除去として採用する。
