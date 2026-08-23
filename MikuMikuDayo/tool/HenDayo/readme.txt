HenDayo

●概要
.dayoファイル編集用のツールです

●目的
dayoファイルは[MikuMikuDayo]～[BinaryDayo]に囲まれたjsonで書かれた部分とそれに続くキーフレームデータを収めたバイナリデータという単純な形式になっていますが、メモ帳でjson部分だけ編集しようと思って普通に編集・保存するとバイナリ部分は破損してしまいます。HenDayoを使うとバイナリ部分を損なう事なくjson部分を編集できます

json部分の内容はソースコードのsaveDayo.hにある、MikuMikuDayoInfo構造体の内容そのものです。

●操作方法

Windows標準のメモ帳と大体似ています
上部の"find"欄に文字列を入力して右の"search"ボタンで検索
上部の"replace to"欄に文字列を入力して右の"replace"ボタンで置換が出来ます


●謝辞
GUIの作成にはOmar Cornut氏のImGuiを
テキストエディタ部分にはBalazsJako氏のImGuiColorTextEditを使わせていただきました。
