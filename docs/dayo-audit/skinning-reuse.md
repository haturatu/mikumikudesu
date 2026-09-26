# 調査: 標準skinningのDayo HLSL再利用

状態: **未実装・調査PR**。基準は `main@7ed41e0` と固定 MikuMikuDayo130。

## 確認した差分

本家 `hlsl/system/DefaultSkinning.hlsl::CS` は `hlsl/skinning.hlsli::DefaultSkinning` を呼び、
`resources_deform.hlsli` のVB/Skin/BoneMatrix/MorphValues/MorphTable/MorphTablePointer/OutBufを使う。
上流helperは位置・UV・追加UV morph、tangent、BDEF/SDEF/QDEFを扱う。

[CMakeLists.txt](../../CMakeLists.txt) は独自の [native_deform.hlsl](../../shaders/native_deform.hlsl) をcompileし、
[VulkanDevice::createNativeDeformPipeline](../../src/graphics/vulkan/vulkan_device.cpp) はその `NativeDeform` を使う。
同shaderはPreviewVertex/BoneTransform/NativeDeformedVertexの別ABIを持ち、LBS/SDEF/QDEFを再実装する。
従ってDayoのskinning.hlsliを取得・同梱しても、この標準deform経路には変更が反映されない。
Preview/CPU側でmorphを処理する経路もあるため、これだけでUV morph全体が動かないとは断定しない。

`CloneCount`自体はNativeSceneDerivedRuntimeでuploadされており、常に未bindという指摘は誤り。
未解決なのは標準deformの上流コード再利用と、各経路を通した数値同等性の検証。

## 対応案

上流resources_deformのABIへモデル入力を揃え、Dayoの `DefaultSkinning` を呼ぶ小さなentry wrapperを
DXCでcompileする。wrapperには実頂点数のbounds checkを置く。本家CSの1024頂点dispatchをそのまま
丸めて使うと末尾threadのout-of-boundsを招く。既存Preview用ABIとの差をbuffer変換で吸収し、
OutBufをBLAS refitとrendererの頂点参照へ接続する。アルゴリズム本体のコピーを増やさない。

## 受け入れ条件

- BDEF1/2/4、SDEF、QDEF、頂点/UV/追加UV morph、tangentの固定fixtureを比較する。
- 頂点数1/1023/1024/1025、clone>1、非表示/空モデル、softbody併用を確認する。
- DXC SPIR-V ABI検証、Vulkan validation、GPU readback、Windows 1.30との差分を記録する。
- portableなPreview fallbackも維持し、上流source欠損時は診断する。

このPRは調査と接続設計のみ。shader置換・Windows/GPU比較は未実施。
