# 未使用の Preview 頂点配列を確保しない

`refreshAnimatedMesh` の `vertices.reserve(vertexCount)` を、アニメーション評価後の
最終的な `rebuildVertices` 判定の内側へ移した。
GPU スキニングで CPU 頂点を生成しないフレームでは、全頂点分の scratch / upstream 確保を行わない。
初期アップロード・トポロジー変更・UV/追加UVモーフ・soft body など、CPU 頂点が必要な経路では確保する。

`application_native_cache` に100,000頂点のモデルと8 MiB以上の PMR upstream 確保の計数を追加。
初期アップロードで実際に大きな確保があることを確認した後、通常フレームでは0回となることを検査する。
main `f10efd8` の実装ではこの検査が失敗し、変更後に成功した。
既存の UV モーフ更新・モデル追加削除・Preview/native 切替の検査も維持する。

これは確保処理の削減であり、同じ量の RSS やメモリ書込が減るという意味ではない。
フレーム時間／FPSの改善率は測定していない。

```sh
cmake --preset linux-release
cmake --build --preset linux-release -j3
ctest --test-dir build/linux-release --output-on-failure
```
