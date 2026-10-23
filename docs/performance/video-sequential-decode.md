# 背景動画の連続デコード

`MediaFile::decodeVideoFrame()` は順送りでデコーダと最後に返した AVFrame を保持する。
同じフレームを選ぶ小数時刻や同一時刻は、そのフレームを再利用する。
逆送り、前回要求から1秒を超えるジャンプ、音声デコードによる共有 demuxer の移動、
デコード失敗の後には seek/flush して再開する。

receive を先に行うことで、1パケットからの複数出力や B フレームの遅延出力を次の要求へ保持する。
EOF では NULL packet を一度送り、残ったフレームを受け取る。
AVPacket / AVFrame の確保をファイルの生存期間へ移し、SwsContext はサイズ・pixel format が同じ間再利用する。
返却する RGBA vector の所有権と「要求時刻以降の最初の PTS を選ぶ」動作は維持する。

## 検証

main `f10efd8` からの変更。`media_decode` は小さな動画をその場で生成し、全画素を比較する。

| 入力 | フレーム数 | 順送り中の seek | 送信パケット数（EOF drain を含む） |
| --- | ---: | ---: | ---: |
| GOP 60 / Bなし | 90 | 1 | 90 |
| Bフレーム / 音声あり | 90 | 1 | 91 |
| 可変フレームレート / Bあり | 38 | 1 | 39 |
| 途中で64×48→96×64へ変化 | 60 | 1 | 60 |

最初の3入力は同じ FFmpeg の bilinear RGBA 出力と完全一致した。
解像度変更入力は、各時刻を独立に seek した参照出力と完全一致し、寸法を ffprobe と照合した。
生成時に実際の B フレーム、可変 PTS 間隔、寸法変更を確認する。
同一時刻・小数時刻・逆送り・大きなジャンプ・EOF後の先頭復帰・音声との交互利用・move・不正時刻も検証する。
旧実装では「連続再生の seek は1回」の検査が失敗する。

前回調査と同じ320×180 GOP 60 の probe では、フレーム0を warmup として1回読み込んだ後の60要求が
1,830パケット／60 seek から59パケット／0 seek になった。warmup がフレーム0を保持するため59パケットとなる。
これは処理回数の削減であり、GPU描画やアプリ全体のFPSの改善率ではない。

ローカルでは通常ビルドと ASan/UBSan で上記4入力を検証した。
CMake経由の `media_decode`、`core`、`audio_export_cli`、`video_export_cli` の4テストも成功した。

```sh
cmake --preset linux-release
cmake --build --preset linux-release --target dayo_media_decode_tests -j3
ctest --test-dir build/linux-release -R '^media_decode$' --output-on-failure
```

`media_decode` は FFmpeg/ffprobe と libx264 を必要とする。テスト用ファイルは一時ディレクトリに生成・削除する。
