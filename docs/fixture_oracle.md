# MikuMikuDayo 1.30 との相互運用テスト

Windows 版 MikuMikuDayo 1.30 が生成したファイルを基準データ（oracle）として、モーション処理、物理演算、ファイル入出力の互換性を検証するための受け入れ条件を示します。

**状態:** このページに示す fixture は計画段階です。Windows 版で生成したバイナリはリポジトリに含まれておらず、比較結果が得られたことを意味しません。

## テスト対象

| Fixture | 確認する内容 | 判定条件 | 状態 |
| --- | --- | --- | --- |
| `camera-parent.vmdayo` | カメラの親モデル・親ボーンと、親が見つからない場合の処理 | 値の一致。未解決の場合は単位行列 | 未実施 |
| `catmull-axis.vmdayo` | ボーンの 4 軸とカメラの 6 軸の補間方式 | `SolverCompatibilityProfile::cameraTolerance` の範囲内 | 未実施 |
| `multi-model.dayo` | 複数モデルの辞書・メタデータ・全トラックとカメラ・ライト | トラック名とキー数の一致 | 未実施 |
| `external-parent.dayo` | 親モデルの削除、未解決親、循環参照 | `Scene::addExternalParent` のエラー内容の一致 | 未実施 |
| `gravity.dayo` | 重力キーと保存設定 | 強度の誤差 1e-3、方向の正規化 | 未実施 |
| `limited-ik.pmx` + `limited-ik.vmdayo` | 1～3 軸の IK 制限、膝・非対称制限 | 位置誤差 1e-3、回転誤差 1e-4 | 未実施 |

## 検証手順

1. Windows 版 1.30 で対象データを保存し、基準 fixture として保管します。
2. Linux 版で読み込み、再シリアライズしてから再読み込みします。値と未知の payload が保持されることを確認します。
3. Linux 版で保存したファイルを Windows 版で開き、逆方向の互換性を確認します。
4. 各 fixture に、生成に使った Release、操作手順、使用モデルの出典と再配布条件、OS・GPU・ドライバなどの検証環境を記録します。

**注意:** Linux 版が生成したファイルを、Windows 版が生成した fixture の代わりとして扱わないでください。双方で同じ実装を使った往復試験だけでは、上流との互換性を検証できません。

## テストの実装箇所

- `tools/capability_suite --list`: テストスイートの一覧
- `MotionSolver::compareBones` / `MotionSolver::compareCamera`: 数値比較
- `PhysicsStepper` / `PhysicsCompatibilityProfile`: 物理演算の固定ステップ処理
- `tests/editor_physics_io_tests.cpp`: 画像連番や HDR などのテスト

関連する互換性の状況は [MikuMikuDayo 1.30 互換性](upstream-1.30.md)を参照してください。
