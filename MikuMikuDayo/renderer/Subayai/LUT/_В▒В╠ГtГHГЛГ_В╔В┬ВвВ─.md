# subayai用のLUTを作成するためのエフェクトを置いとくフォルダ

LUTを作るためのコードはそれなりに重いので前計算用にのみ使う事。LUTが何の略だと思っとるんダヨー？

## RampLUT.fx
Rampテクスチャ用のddsファイルを作るモノ  
Chrestensen-Burleyモデルに基づいてBSSRDFを作りそれを球面上で積分した結果をRampテクスチャ(左上に表示)に書き込みます

### 使い方
1. RampLUT.pmxを読み込む
1. 肌の調整に使いたいモデルを読み込み、肌マテリアルに`RampLUT_material.txt`を割り当てる
1. モーフを調整してお肌の具合を整える
1. FXDebugウィンドウからエフェクト`RamLUT`を選択
1. リソース`RampLUT`を選択して`dump`ボタンを押すとRampLUTに指定するテクスチャを保存
1. リソース`RampZH`を選択して`dump`ボタンを押すとRampZHに指定するテクスチャを保存
1. モーフの値に対応する材質注釈ファイルを手作業で書いてできあがり

### RampLUT.pmxのモーフ

#### RampLUT/ZHを作るためのパラメータ
| | |
|--|--|
|AlbedoR/G/B| Albedoの想定値
|SigmaTR/G/B| 消散係数[1/mm]

#### 材質注釈に指定されるパラメータ
| | |
|--|--|
|CatR/G/B/A| Catに指定する値...SSSSSの伸び具合


| | |
|--|--|
|Roughness| Roughnessに指定する値
|IOR| IORに指定する値
|PowR/G/B| ColorConstでpowモードにした時のpowに指定する値

| | |
|--|--|
|NormalScale| NormalScaleに指定する値
|TextureLoops| TextureLoopsに指定する値