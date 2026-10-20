# #312: DDSのブロック展開を高速化する

colorPaletteは元からブロックごとに1回だった。テクスチャ全体へ共有すると異なる端点色が壊れるため採用しない。
形式をslice冒頭で分岐し、BC1/2/3/4/5それぞれをコンパイル時に特殊化する。
4×4ブロックの有効な行/列を先に決め、画素ごとの範囲判定を除く。
BC3/4/5の48bit alpha selectorをブロックごとに一度復元する。
符号付きBC4/5も画素ごとのfloatパレット/selector再計算をブロック/チャンネルごとの計算へ移す。
整数補間・符号端点の-128処理・float演算順序・RGBA/float形式を維持する。

## 検証

- 単体テスト140条件: 全7形式、フルブロック/端数サイズ、異なる端点の隣接ブロック、
  BC1透明色、BC2 alpha全16値、BC3/4/5の全selectorと両端点順、signed端点と補間を既知の色/float値と比較。
  各条件で末尾を切り詰めたpayloadの拒否も確認。
- 既存coreテスト: 非圧縮/typed DDS、volume/cube/mip、allocation budgetと不正入力を維持。
- 合成70ファイル（1〜1024px、odd寸法、全形式、cube配列+MIP、volume+MIP）とTeto実画像7ファイルで、
  変更前後のformat・decoded byte数・全画素FNV-1aハッシュが3回とも一致。
- ReleaseとASan/UBSanのcore/image_blocksテスト成功。clang-tidy成功。

## 時間

GCC -O3 -DNDEBUG、同じソース構成の変更前/後decoderを別バイナリへビルド。
OSキャッシュを温め、1ファイル10回読み込みの中央値を3回ずつ交互に計測し、その中央値を比較。
入力ファイルread・全MIP decode・出力確保を含む。GPUアップロードや通常再生は含まない。

| 入力 | 前 ms | 後 ms |
| --- | --- | --- |
| BC1 1024² | 2.1105 | 1.6997 |
| BC2 1024² | 5.1878 | 2.6530 |
| BC3 1024² | 5.5987 | 2.8773 |
| BC4 UNORM 1024² | 3.4692 | 2.6852 |
| BC5 UNORM 1024² | 3.9196 | 3.1359 |
| BC4 SNORM 1024² | 10.4147 | 1.7986 |
| BC5 SNORM 1024² | 19.2241 | 3.4786 |
| Teto face2+4.dds（全MIP） | 7.0276 | 4.0381 |
| Teto tdatest5.dds（全MIP） | 27.8371 | 15.9336 |

初回の単発計測ではBC5 UNORMの改善が見られず、ばらつきがあった。上表は対比較を繰り返した値であり、
特定CPUでのロード処理の観測値。再生FPSの改善率としては扱わない。
明示的SIMD・スレッド並列化・追加の画像キャッシュは導入せず、基本処理の重複除去を採用する。
圧縮データのVulkanへの直接転送は画像形式とGPUリソース管理の変更を伴うため、このCPU decoder変更には含めない。

## 再現

`generate_dds_fixtures.py OUTPUT`でseed固定の合成データを生成する。
`texture_decode_benchmark.cpp`に複数のDDSパスを渡すとformat/byte数/hash/10回中央値をTSV出力する。
変更前は別worktreeのimage.cpp/image_hdr.cppを同じオプションでリンクする。

```sh
python docs/performance/generate_dds_fixtures.py /tmp/dds-fixtures
c++ -O3 -DNDEBUG -std=c++20 -I src -I build/linux-release/_deps/stb-src \
  docs/performance/texture_decode_benchmark.cpp src/core/image.cpp src/core/image_hdr.cpp -o /tmp/texture-bench
/tmp/texture-bench /tmp/dds-fixtures/*.dds assets/models/teto/model/tex/*.dds
```
