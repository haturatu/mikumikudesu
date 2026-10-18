# トラブルシューティング

問題が再現する場合、使用した preset、GPU と Mesa / Vulkan driver、起動引数、stderr のエラー、入力素材の種類を確認してください。RT renderer のフォールバックは、必ずしも Preview の起動失敗を意味しません。

## ビルドできない

### `external/libmmd` が見つからない

Git submodule を初期化してください。

```sh
git submodule update --init --recursive
make setup
```

### SDL3、Vulkan、shader compiler などが見つからない

[インストール](installation.md)の必須パッケージを確認してください。通常 preset では依存ライブラリの一部を CMake が取得しますが、SDL3、Vulkan driver、shader compiler などのシステム要件は別途必要です。

`linux-system-only` では `DAYO_FETCH_DEPENDENCIES=OFF` が設定されています。必要な開発用 package をインストールするか、通常 preset を使用してください。

### MikuMikuDayo のファイルがない

`make` は上流アセットを取得しません。取得と検証を実行します。

```sh
make setup
```

ダウンロード先と展開物の内容は `deps/mikumikudayo.lock`、`deps/mikumikudayo-runtime.manifest`、`scripts/fetch-mikumikudayo.py` で管理されています。ダウンロードが失敗したときは、ネットワーク、ローカルキャッシュ、digest 検証エラーを確認してください。

## 起動・描画できない

### Vulkan のデバイスが見つからない

`vulkaninfo` で loader、ICD、デバイスを確認します。

```sh
vulkaninfo --summary
./build/linux-debug/mikumikudesu --probe --hidden
```

Vulkan 1.3 に対応するドライバと swapchain が必要です。AMD では Mesa RADV の利用を推奨します。Window system の起動に失敗する場合は、ディスプレイセッションと SDL3 を確認してください。

### Subayai / BDPT を選んでも Preview になる

この動作は設計上のフォールバックです。GPU の RT feature、FX graph の要求、native runtime の初期化を確認してください。機能のない GPU では Preview を使用します。詳細は[レンダラ](renderers.md)と[互換性](upstream-1.30.md)を参照してください。

### カメラをドラッグしても動かない

Viewport 上にカーソルがあることを確認してください。`Follow VMD camera` が ON の場合、手動操作はロックされます。OFF にしてから操作するか、`Reset view` を使用してください。詳しくは[GUI の使い方](usage.md)を参照してください。

## 音声・動画を出力できない

- `--export-m4a` は Vulkan 不要ですが、入力音声とエンコーダの利用が必要です。
- `--export-video` は Vulkan と Preview renderer を使います。完全なディスプレイ非依存の headless 実行ではありません。
- 入力音声が複数ある場合は `--audio-source PATH` で選択してください。
- 出力先に既存ファイルがある場合は、上書きを意図する場合にのみ `--overwrite` を指定してください。
- 設定の組み合わせについては[CLI リファレンス](cli.md)を参照してください。

## 不具合を報告する

再現手順、実行したコマンド、`--probe` の結果、エラーの全文、対象モデルやモーションの形式、期待結果と実際の結果を含めてください。再配布できないモデルやモーションをそのまま添付しないでください。
