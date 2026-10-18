# mikumikudesu

mikumikudesu は GNU/Linux 向けの MMD モデル・モーションの編集、再生、レンダリングを行うネイティブアプリケーションです。SDL3、Vulkan 1.3、ImGui を使用します。AMD GPU では Mesa RADV を推奨します。

MikuMikuDayo 1.30 のデータとエフェクトを基準に開発しています。すべてのエフェクトや描画結果が本家と一致するわけではありません。通常の描画には Preview を使用し、Subayai / BDPT は GPU 機能とエフェクトの条件に応じて有効化されます。未対応時には Preview へフォールバックします。対応状況は[互換性と制限事項](docs/upstream-1.30.md)を参照してください。

## クイックスタート

Linux x86_64、Vulkan 1.3 対応ドライバ、CMake 3.25 以上、Ninja、C++20 コンパイラ、SDL3、shader compiler が必要です。[インストール](docs/installation.md)にパッケージと依存関係を記載しています。

```sh
git clone --recurse-submodules https://github.com/haturatu/mikumikudesu.git
cd mikumikudesu
make setup
make
./build/linux-debug/mikumikudesu --asset /path/to/model.pmx
```

`make setup` は固定した MikuMikuDayo 1.30 の配布物をダウンロードして検証します。`make` だけでは取得しません。モデルとモーションは利用条件を確認したうえで別途用意してください。

## 基本操作

- Viewport: 左ドラッグで回転、右ドラッグで平行移動、ホイールでズーム
- Animation: キーフレームの選択・移動・編集、Undo / Redo
- Inspector: モデル、カメラ、ライト、Scene の編集
- ファイル: PMX / VMD などの読込、`.dayo` プロジェクト、音声・映像の書き出し

操作手順は[GUI の使い方](docs/usage.md)、引数と出力例は[CLI リファレンス](docs/cli.md)を参照してください。

## ドキュメント

| 項目 | 内容 |
| --- | --- |
| [ドキュメント一覧](docs/README.md) | 利用者・開発者向けの索引 |
| [インストール](docs/installation.md) | 必要環境、依存パッケージ、ビルド、実行 |
| [GUI の使い方](docs/usage.md) | モデル、モーション、カメラ、ライト、メディア |
| [CLI リファレンス](docs/cli.md) | 起動引数、GPU probe、音声・動画出力 |
| [レンダラ](docs/renderers.md) | Preview、Subayai / BDPT、画質と制限 |
| [トラブルシューティング](docs/troubleshooting.md) | 起動、依存関係、Vulkan、出力の切り分け |
| [開発者向け情報](docs/development.md) | テスト、最適化 preset、構成 |

## ライセンスと配布物

本リポジトリのソースコードは [MIT License](LICENSE) です。セットアップ時に取得する MikuMikuDayo と、別途入手した PMX / VMD / 画像・音声素材にはそれぞれの利用条件が適用されます。

MikuMikuDayo 1.30 は `deps/mikumikudayo.lock` に固定した[配布物](https://github.com/pennennennennennenem/MikuMikuDayo/releases/tag/MikuMikuDayo130)を使用し、`MikuMikuDayo/` は Git で管理しません。再配布時は配布物に含まれるライセンス表示を保持してください。
