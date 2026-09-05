# MikuMikuDayo 1.30互換性

## この更新の範囲

公式[1.30 Release](https://github.com/pennennennennennenem/MikuMikuDayo/releases/tag/MikuMikuDayo130)を
テスト・配布の基準にする第1段階です。Release APIのasset digestと取得したZIPのSHA256を照合します。
`deps/mikumikudayo.lock`が取得元の正本で、アプリ自身のバージョンとは独立しています。

`deps/mikumikudayo-runtime.manifest`はrelease rootからの相対パスを1行ずつ記述します。
末尾`/`はディレクトリ全体、それ以外は単一ファイルです。空行と`#`コメントを許可します。
fetchは全エントリと主要renderer・画像・ライセンスの存在を検証し、成功した場合だけ置換します。
installも同じ一覧を使い、拡張子による除外を行わず、第三者ライセンスとtexture/importを保持します。

## 対応段階

| 段階 | 完了条件 | 状態 |
| --- | --- | --- |
| 1. baseline・配布 | 1.30 ZIPの固定、SHA256検証、共通manifestによる配布、公式assetの既存互換テスト | このPRの対象 |
| 2. データ・solver互換 | Windows 1.30保存fixtureの往復、camera/external parent/制限IKの数値比較 | データ形式は実装済み、実機検証は未実施 |
| 3. FX 1.30契約 | buffer/size expression、pow、CloneCount/CLONEDVERTEXCOUNT、MatDescとresource allocationの接続 | 未完了 |
| 4. Subayai/BDPT実行 | Vulkan BLAS/TLAS/SBT、各pass実行器、RT対応GPUでの画像比較 | graph解析・feature検出のみ |

`nativeSubayai`/`nativeBdpt`はfalseのままです。Previewのclone複製は、上流FXが参照する
`Dayo::CloneCount[modelIndex]`との接続完了を意味しません。1.30の追加エフェクトを同梱しても、
その実行をサポートしたことにはなりません。OIDNは任意検出で、2.5.0への固定は行いません。
非同期画像出力は既存実装を使用し、bounded queueとファイル名末尾からの連番開始は別の残件です。

## Windows fixtureの受け入れ条件

Windows 1.30実行環境での保存・再読込は今回のLinux検証に含みません。
独自serializerで生成したファイルを「本家が生成したfixture」として扱わないでください。
追加するfixtureには、生成に使ったRelease、操作手順、モデルの出典・再配布条件を記録します。

- `camera-parent.vmdayo`: parentModel/parentBone/parentBoneNameの一致・欠落・再解決
- `catmull-axis.vmdayo`: boneの4軸、cameraの6軸それぞれの補間method
- `multi-model.dayo`: camera/light subsetと複数モデルsubset、dictionary、metadata、全track
- `external-parent.dayo`: 親モデル削除、未解決親、循環参照
- `gravity.dayo`: 重力keyと保存設定

本家save → native load → native serialize → native reloadで上記項目と未知payloadの保持を検証し、
続いてnative save → 本家loadを確認します。制限IKは1/2/3軸、膝型・非対称limitを持つ小さいPMXを
同じframeで評価し、bone matrixとendpointの誤差、許容値、実行環境を記録します。

## 再現コマンド

```bash
python3 scripts/fetch-mikumikudayo.py
cmake --preset linux-debug -DDAYO_WARNINGS_AS_ERRORS=ON
cmake --build --preset linux-debug
ctest --preset linux-debug --output-on-failure -R '^(core|upstream_fetch|upstream_compat)$'
cmake --install build/linux-debug --prefix /tmp/mikumikudesu-130-package
```

`upstream_fetch`はネットワーク不要の合成ZIPによるinstaller/packagingの回帰テストです。
`upstream_compat`は取得済み公式assetによるgraph展開、画像読込、VMD/PMXの互換テストです。
Windows実機の相互運用結果とは区別します。
