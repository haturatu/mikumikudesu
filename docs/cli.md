# CLI リファレンス

コマンドはビルドした `mikumikudesu` を直接実行します。以下は Debug preset の例です。

```sh
./build/linux-debug/mikumikudesu --help
./build/linux-debug/mikumikudesu --asset model.pmx --asset motion.vmd
./build/linux-debug/mikumikudesu --probe --hidden
```

`--asset` は繰り返し指定できます。`--model` は同じ引数の別名です。引数を伴わないファイルパスもアセットとして扱います。相対パスは起動時の作業ディレクトリから解決されます。

## 起動・描画

| オプション | 説明 |
| --- | --- |
| `--asset PATH` / `--model PATH` | 起動時にファイルを読み込む。複数指定可 |
| `--save-project PATH` | プロジェクトを `.dayo` で保存 |
| `--probe` | GPU / renderer の機能を出力して終了 |
| `--hidden` | ウィンドウを非表示で作成 |
| `--frames N` | N フレーム描画後に終了。N は正の整数 |
| `--no-validation` | Vulkan validation を無効化 |
| `--renderer preview|subayai|bdpt` | 描画モード。既定値 `preview` |
| `--preview-quality auto|apu|high` | Preview の画質。既定値 `auto` |
| `--present fifo|fifo-relaxed|mailbox|immediate` | Present mode。既定値 `fifo` |
| `--async-compute` | 実験的な Preview 変形処理の別キュー実行 |

`|` は指定可能な値の区切りです。Subayai / BDPT を要求しても、GPU と FX graph が条件を満たさなければ Preview にフォールバックします。利用可能な renderer を確認するには `--probe` を使ってください。[レンダラ](renderers.md)も参照してください。

## 音声書き出し

| オプション | 説明 |
| --- | --- |
| `--export-m4a PATH` | M4A / AAC を書き出す |
| `--audio-source PATH` | 音声の入力を明示する |
| `--audio-bitrate KBPS` | AAC bitrate。既定値 192 |
| `--audio-from SEC` / `--audio-to SEC` | 出力する時間範囲。M4A のみ |
| `--overwrite` | 出力先が存在する場合に置き換える |

`--export-m4a` は Vulkan やウィンドウの初期化を必要としません。

```sh
./build/linux-debug/mikumikudesu --asset music.wav --export-m4a music.m4a
./build/linux-debug/mikumikudesu --audio-source music.wav \
  --export-m4a chorus.m4a --audio-bitrate 256 --audio-from 35 --audio-to 78.5
```

## 動画書き出し

| オプション | 説明 |
| --- | --- |
| `--export-video PATH` | MP4 を書き出す |
| `--video-width PX` / `--video-height PX` | 出力解像度。既定値 1920 × 1080 |
| `--video-fps FPS` | 出力 FPS。既定値 30 |
| `--video-codec h264|h265|hevc|av1` | codec。既定値 `h264`。`hevc` は `h265` の別名 |
| `--video-bitrate KBPS` | 映像 bitrate。既定値 8000 |
| `--video-hardware` | 利用可能なら hardware encoder を優先 |
| `--video-from-frame N` / `--video-to-frame N` | 出力フレーム範囲。0 始まり |
| `--audio-source PATH` | 音声入力を明示する |
| `--audio-bitrate KBPS` | AAC bitrate。既定値 192 |
| `--no-audio` | MP4 に音声を含めない |
| `--overwrite` | 既存の出力先を置き換える |

```sh
./build/linux-debug/mikumikudesu \
  --asset miku.pmx --asset dance.vmd --asset camera.vmd --asset music.wav \
  --export-video dance.mp4 --video-width 1920 --video-height 1080 \
  --video-fps 30 --video-codec h264 --video-bitrate 8000

./build/linux-debug/mikumikudesu \
  --asset miku.pmx --asset dance.vmd \
  --export-video preview.mp4 --video-to-frame 300 --no-audio
```

動画出力は Vulkan の hidden window と Preview renderer を使います。`--video-fps` の変更はモーションの再生速度を変更しません。GUI は出力しません。音声入力が複数ある場合は `--audio-source` を指定してください。出力完了までは `.part` ファイルを使用します。

## 引数の組み合わせ

- `--export-m4a` と `--export-video` は同時に指定できません。
- `--video-*` と `--no-audio` は `--export-video` が必要です。
- `--audio-source`、`--audio-bitrate`、`--overwrite` はどちらかの export 指定が必要です。
- `--audio-from`、`--audio-to` は `--export-m4a` でのみ使用できます。
- 解像度、bitrate、FPS、描画フレーム数には正の値を指定します。

## 関連項目

- [インストール](installation.md)
- [GUI の使い方](usage.md)
- [トラブルシューティング](troubleshooting.md)
