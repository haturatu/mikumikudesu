# Tools (PR6 split skeleton, OFF by default)

```cmake
cmake --preset linux-debug -DDAYO_ENABLE_TOOLS=ON
cmake --build --preset linux-debug --target fxedit sequence_movie vdb2dds capability_suite
```

| tool | purpose | status |
| --- | --- | --- |
| `fxedit` | headless project/motion check entry point | skeleton |
| `sequence_movie` | `SequencePathSpec` plan + bounded queue contract probe | skeleton |
| `vdb2dds` | VDB -> DDS conversion entry point | skeleton (no voxel sampling yet) |
| `capability_suite` | fixture/oracle suite enumeration (`--list`) | stub |

All tools log via `dayo::log` (DEBUG/INFO to stdout, WARN/ERROR to stderr).
