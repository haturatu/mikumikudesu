# 調査: textureの明示HLSL型とファイル形式の保持

状態: **実装済み**。基準は `main@7ed41e0` と固定 MikuMikuDayo130。

本家 `src/YRZFx.ixx::FXRes::type` はTexture2D/3Dにも適用される。
`createTextureLambda` / `createTexture3DLambda` はtypeが空の場合だけ実DXGI formatから型を推定し、
明示型は保持する。ファイル読込後は実リソースのformat/sizeを宣言へ反映する。

[EffectTexture](../../src/core/effect.hpp) にはtype fieldがなく、
[texture parser](../../src/core/effect.cpp) もtypeを読まない。
[shader source生成](../../src/fx/fx_shader_source.cpp) はformatだけからfloat/float2/float4を推定する。
従って、明示typeを与えた定義はparser→shaderの経路で保持されない。

外部画像は [loadDdsImageRgba8](../../src/core/image.cpp) 等でRGBA8へ変換し、
[FxResourceRuntime](../../src/graphics/fx_resource_runtime.cpp) もそのupload形式を使う。
DDSのfloat/HDR等のDXGI形式は対応表にないものを拒否する。BC4/BC5のsigned形式を含む
数値意味論やcolor spaceの完全保持も、RGBA8表示用decodeとtyped FX resourceでは別途検証が必要。

外部画像のsize/format優先順位を直すPR #246は、この形式保持や明示typeの再現を解決しない。
RGBA8 decoderへ渡したデータをRGBA8 shader/attachmentに揃える修正である。

対応は、まずEffectTexture.typeをIRまで保持して対応formatとの組合せを検証すること。
次にtyped pixel payloadとformatを保つdecoder/upload ABIを用意し、SRV/UAVのcomponent typeと
image formatを一致させる。対応外の明示型を黙ってfloat4に変えず、具体的な診断にする。

受け入れ条件は、明示型あり/なしのHLSL生成とSPIR-V reflection、float/HDR/uint/signedの
fixture、2D/3D各mip、数値のGPU readback、対応外組合せのエラー。

## 実装

- `EffectTexture.type` を parser から HLSL 生成まで保持する。float/int/uint の1〜4成分を認識し、数値型の不一致を診断する。
- `loadTextureImage` は DDS の8/16/32 bit、1/2/4成分の対応形式を保持する。float、UNORM、SNORM、UINT、SINT、sRGB と旧形式の浮動小数点 DDS を扱う。
- HDR は float32、signed BC4/BC5 は符号付き正規化値を float32 に復号する。BC1〜5 UNORM は既存の復号器を再利用する。
- Vulkan の形式、転送サイズ、shader型、attachment形式を実データに揃える。外部画像の各mip・3D depthを保持する。
- テクスチャを upload 前に所有対象へ登録し、upload失敗時も解放する。

既存の表示用 RGBA8 API は維持。未対応DXGI形式、パディング付き typed DDS row pitch、FX宣言で表現できないarray/cubeは明示的に拒否する。

ビルド: Linux `mikumikudesu`。テスト・GPU readback・Windows実行は今回行っていない。前提PR: #251。
