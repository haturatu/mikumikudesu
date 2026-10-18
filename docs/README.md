# ドキュメント

mikumikudesu のインストール、操作、設定、開発・検証に関する文書です。通常の利用は上から順に参照してください。

## 利用者向け

| ページ | 内容 |
| --- | --- |
| [インストール](installation.md) | 必要環境、Arch / Artix の導入例、ビルドと実行 |
| [GUI の使い方](usage.md) | モデル、モーション、Viewport、カメラ・ライト、メディア |
| [CLI リファレンス](cli.md) | 引数、デバイス診断、M4A・MP4 の書き出し |
| [レンダラ](renderers.md) | Preview の画質設定、RT レンダラの利用条件 |
| [トラブルシューティング](troubleshooting.md) | よくある問題の確認と対処 |

## 開発・検証

| ページ | 内容 |
| --- | --- |
| [開発者向け情報](development.md) | CMake preset、テスト、構成、性能最適化 |
| [Preview 性能](preview-performance.md) | GPU メモリ、キャッシュ、非同期 compute、測定結果 |
| [MikuMikuDayo 1.30 互換性](upstream-1.30.md) | 移植範囲、条件、未検証項目 |
| [Dayo 互換性調査](dayo-audit/README.md) | エフェクトとリソースの実装・差分調査 |
| [Windows fixture 受け入れ条件](fixture_oracle.md) | 数値比較・相互運用テストの計画 |
| [キーフレームエディタの検証](ui-ux/keyframe-editor.md) | 操作とテストの記録 |
| [ゼロ法線回帰テスト](preview/zero-normal-regression.md) | Preview の位置・法線処理の検証 |

## 記載方針

- コマンド例は、特記がなければリポジトリのルートディレクトリから実行します。
- `build/linux-debug/` は Debug preset の出力先です。Release を使う場合は対応する preset とパスに読み替えます。
- `Preview` で利用できる機能と `Subayai / BDPT` に必要な RT 機能は分けて説明します。
- 日付やコミットが記載された調査・実測文書は、その時点の結果です。現在の実装や CI 結果と区別してください。
