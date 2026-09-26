# 調査: textureの明示HLSL型とファイル形式の保持

状態: **未実装・調査PR**。基準は `main@7ed41e0` と固定 MikuMikuDayo130。

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
本PRはソース比較のみで、type field追加や新しいpixel decoderは実装していない。
