# GUI の使い方

PMX モデルと VMD モーションを読み込むと、Scene のプレビューとタイムラインを編集できます。複数の `--asset` を指定して起動することも、ファイルをドラッグ＆ドロップすることもできます。オプションとコマンド例は[CLI リファレンス](cli.md)を参照してください。

## Viewportとカメラ・ライトの編集

Viewportの画像上で左ドラッグすると回転、右ドラッグすると平行移動、ホイールでズームします。
右クリックのメニュー、または画像上部の`Reset view`で視点をリセットできます。
カメラVMDがある場合、`Follow VMD camera`がONの間は手動操作をロックします。
OFFにすると現在の視点と画角を維持して操作でき、リセットでもロックを解除します。
Pan・Zoomの操作量はMMD座標に換算するため、モデルの正規化スケールに依存しません。

画像上部の`Camera` / `Light`を押すと、現在のワークスペースのInspectorで編集できます。
Inspector上部の`Selection` / `Camera` / `Light`でも切り替えられます。
編集欄は表示中のカメラ・ライトの値を取得し、変更は即座にプレビューへ反映します。
`Register camera key` / `Register light key`で現在フレームに保存し、`Ctrl+Z`で登録を取り消せます。

設定を再利用するには、キーを登録してからInspectorの`VMD file`に保存先を指定し、
`Export camera VMD` / `Export light VMD`を押します。読み込むときは同じ欄にファイルを指定して
`Import camera keys` / `Import light keys`を押します。読み込みは対象トラックを置き換え、
ほかのトラックを保持します。登録前のプレビュー変更はエクスポートに含まれません。

## キーフレームの編集

上部の`Animation`へ切り替えると、タイムラインとInspectorを並べて編集できます。
トラック名の検索には日本語名と、モデルに設定されている英語名を使えます。種別の欄でボーン・
モーフ・カメラなどを絞り込み、ダイヤ形のキーをクリックして選択します。選択キーは金色になります。

キーをドラッグすると移動先が表示され、離すと1回のUndo単位で確定します。`Esc`でキャンセル、
`Ctrl`クリックで選択の追加・解除、`Shift`クリックで同じトラックの範囲選択、空白からのドラッグで
矩形選択ができます。`Ctrl+A`は検索条件に合うキーをすべて選択します。`-1f` / `+1f`または左右キーで
1フレーム、`Shift`+左右キーで10フレーム移動できます。

Inspectorには選択したキーの現在値が表示されます。フレームや位置、回転、モーフの重みなどを変更し、
`Apply key`で反映します。複数選択では`Offset (frames)`に移動量を入力し、`Move selected keys`で
まとめて移動できます。`Ctrl+Z` / `Ctrl+Y`で取り消し・やり直し、`Clear key selection`でモデルの設定へ戻ります。

目盛りのクリック・ドラッグで再生位置を指定できます。`Ctrl`+ホイールでポインターを中心にズーム、
中ボタンドラッグまたは`Shift`+ホイールで横移動、`Fit`で全体表示に戻ります。`Frame`欄と
`Prev key` / `Next key`でも移動できます。`Playback / media`に再生速度・ループ・音声設定、
`More...`に補間モードとVMDの書き出しをまとめています。

画面の比較と検証内容は[UI/UX調査記録](ui-ux/keyframe-editor.md)を参照してください。

## MP4を音声として使う

MP4をD&Dすると、音声だけを読み込み、波形を表示してタイムラインと同期再生します。
映像decoderと背景フレームのGPU転送は、背景動画を明示的に選ぶまで実行しません。
音声を含まないMP4は、音声読み込みエラーとして表示します。

読み込んだMP4の映像は、上部の「Media」メニュー、またはTimelineの「Media」欄にある
「Show MP4 video」でON/OFFを切り替えます。モデル選択中も操作できます。
同じ欄でVolume・Audio offsetを変更でき、操作エラーも表示されます。
別の背景動画を指定する場合はSceneのInspectorで「Video file」を入力し、
「Apply video file」を押します。OFF中は動画のdecode・GPU転送を止め、
読み込んだ動画とタイムラインは保持します。再度ONにすると現在時刻のフレームへseekします。
OFF時は読み込んだ背景画像があれば画像、なければ白い背景へ戻ります。
背景動画と音声は独立しているため、別の音源に差し替えても指定済みの背景を維持します。
音声と背景の指定はプロジェクトに保存され、音声はseek・ループ・再読み込み時に
タイムラインの位置とAudio offsetから再生位置を求めます。再生速度の変更は音声にも反映します（ピッチも変化します）。

## モデルとモーションの読込例

動作確認用として、配布元のアーカイブをローカルの`assets/models/`へ取得できます。
アーカイブは再配布しない前提で`.gitignore`対象にし、各モデルのREADMEとテクスチャは展開したまま保持しています。

- `assets/models/miku/model/miku.pmx` — Tda式初音ミク デフォ服ver（[BowlRoll #16344](https://bowlroll.net/file/16344)、作者: 金子卵黄/Tda。ReadMeの利用条件を優先）
- `assets/models/teto/model/teto.pmx` — Tda式重音テトTypeS（[BowlRoll #11308](https://bowlroll.net/file/11308)、作者: Tda/やまもと。ReadMeの利用条件を優先）

起動例:

```bash
./build/linux-debug/mikumikudesu --asset assets/models/miku/model/miku.pmx
./build/linux-debug/mikumikudesu --asset assets/models/teto/model/teto.pmx
# 複数モデル + motion + 背景を同一Sceneへ読み込むこともできます
./build/linux-debug/mikumikudesu \
  --asset assets/models/miku/model/miku.pmx \
  --asset assets/models/teto/model/teto.pmx \
  --asset motion.vmd --asset background.png
```

同じモデルへ複数のVMDを読み込むと、既存のキーを保持しながら追加します。ダンス用VMDの後に
表情用VMD（目線のボーンキーを含む場合も可）を読み込んでも、ダンスは消えません。同じボーン名・
モーフ名の同じフレームにキーがある場合は、後から読み込んだ値を使います。

重音テトを使った作品の公開や改変時は、[重音テト公式ガイドライン](https://kasaneteto.jp/guidelines/)と各モデル付属READMEを確認してください。

## 表示言語

上部の **Language / 言語 / 语言 / Язык** メニューから、English（米国英語）、日本語、简体中文（簡体字中国語）、Русский（ロシア語）を切り替えられます。初回はOSの優先言語を使用し、未対応の場合は英語になります。選択はSDLのユーザーデータフォルダー内の `haturatu/mikumikudesu/language.txt`（Linuxでは通常 `~/.local/share/haturatu/mikumikudesu/language.txt`） に保存され、プロジェクトとは独立して次回起動時も適用されます。

モデル名・ボーン名・モーフ名・ファイルパスは元のデータを表示します。数値入力と保存形式は従来どおり小数点に `.` を使用します。開発者向けログと外部ライブラリの詳細診断は原文で表示します。

## 関連項目

- [CLI リファレンス](cli.md)
- [キーフレーム編集の検証記録](ui-ux/keyframe-editor.md)
- [トラブルシューティング](troubleshooting.md)
