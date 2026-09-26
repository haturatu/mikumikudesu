# 調査: Dayo連番名とoutputFileのruntime接続

状態: **実装済み**。基準は `main@7ed41e0` と固定 MikuMikuDayo130。

本家 `src/YRZ.ixx::SeqFilename` はstem末尾の数字を抽出し、`max(initialNumber, trail)` から
指定桁数で番号を作る。既存ファイルは次番号へ進める。`dayo.cpp` の録画処理は
`g_edi.outputFile` / `recordNum` / 5桁を同関数へ渡している。

[OutputSettings/outputPath](../../src/core/output.cpp) は `filenamePattern` にframe番号を渡す。
[Application](../../src/app/application.cpp) はprojectの録画範囲・解像度・sampleを適用するが、
`project.editor.outputFile` をimage sequenceの命名へ接続していない。
[sequence_path.cpp](../../src/core/sequence_path.cpp) の末尾番号parserはmovie入力用であり、
存在しているだけでは画像出力にbindされたことにならない。

対応は描画frame番号と出力連番を分け、outputFileのdirectory/stem/末尾番号/拡張子を保存・復元する。
本家の5桁、既存ファイル回避を再現する互換モードと、desuの明示overwrite設定の意味を定義する。
UIには初回出力名を表示する。100%等の文字をprintfのformat文字列へそのまま渡さず、
構造化したprefix・番号・桁数から組み立てる。既存sequence_pathの固定長buffer/番号overflowも検討対象。

受け入れ例: `shot0042.png`から開始、recordStart!=0、末尾数字なし、数字だけのstem、
既存番号の穴、Unicode/長いprefix/percent、uint32境界、非同期queueでの一意な命名。
`.dayo` load→出力→save→reloadでoutputFileが実際の命名と一致することを確認する。

## 実装

OutputSettings.sequenceFile を指定した経路では末尾番号から5桁で開始し、scene frameとは独立した
worker内の番号を使う。既存番号はスキップする。overwrite=trueなら指定番号を上書きする。
未指定のCLI/既存呼出しは従来のframe番号とfilenamePatternを維持するが、printf呼出しを安全なparserへ置換した。

非上書き出力は同一filesystemの一時ファイルへencode後、hard linkで保存先を作る。
他processと衝突しても既存fileを置換せず、Dayoモードでは次番号へ進む。hard link非対応filesystemはIOエラーを返す。
uint32番号枯渇は明示エラーとし、数字だけのstem・percent・Unicodeを扱う。固定長256byte formatterは廃止した。

UIのFirst filename/Directory/Formatとproject.editor.outputFileを接続し、保存・読込・new project時の状態も同期する。
初回出力名をUIに表示する。sequence_path.cppをdayo_coreへ登録し、入力/出力で共通利用する。

Linux `mikumikudesu` ビルド成功。今回テスト・実出力検証は行っていない。前提PR: #249。
