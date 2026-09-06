#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ! -f "$project_dir/external/libmmd/CMakeLists.txt" ]]; then
  printf '[ERROR] libmmd submodule is not initialized\n' >&2
  printf '[ERROR] run: git submodule update --init --recursive\n' >&2
  exit 1
fi
test_dir=$(mktemp -d -t mikumikudesu-amd-test.XXXXXX)
trap 'rm -rf -- "$test_dir"' EXIT

printf '[INFO] Checking Vulkan/RADV environment\n'
if ! command -v vulkaninfo >/dev/null 2>&1; then
  printf '[ERROR] vulkaninfo is required (install vulkan-tools)\n' >&2
  exit 1
fi
vulkaninfo --summary

printf '[INFO] Preparing pinned MikuMikuDayo test assets\n'
python3 "$project_dir/scripts/fetch-mikumikudayo.py"
printf '[INFO] Configuring linux-debug preset\n'
cmake --preset linux-debug -S "$project_dir"
printf '[INFO] Building mikumikudesu\n'
cmake --build --preset linux-debug
printf '[INFO] Running CTest suite\n'
ctest --preset linux-debug --output-on-failure --parallel "$(nproc)"
printf '[INFO] Probing renderer features\n'
"$project_dir/build/linux-debug/mikumikudesu" --probe --hidden --renderer bdpt
printf '[INFO] Rendering bundled PMX smoke test\n'
sample_pmx=$(find "$project_dir/MikuMikuDayo/sample" -maxdepth 1 -type f -iname '*.pmx' -size +100k | head -n 1)
sample_vmd=$(find "$project_dir/MikuMikuDayo/sample" -maxdepth 1 -type f -iname '*.vmd' | head -n 1)
sample_image=$(find "$project_dir/MikuMikuDayo/sample" -maxdepth 1 -type f -iname '*.png' | head -n 1)
if [[ -z "$sample_pmx" || -z "$sample_vmd" || -z "$sample_image" ]]; then
  printf '[ERROR] bundled smoke-test assets are missing\n' >&2
  exit 1
fi
"$project_dir/build/linux-debug/mikumikudesu" --hidden --frames 10 \
  --no-validation --asset "$sample_pmx" --asset "$sample_vmd" \
  --save-project "$test_dir/roundtrip.dayo"
printf '[INFO] Loading native .dayo round trip\n'
"$project_dir/build/linux-debug/mikumikudesu" --hidden --frames 3 --no-validation \
  --asset "$test_dir/roundtrip.dayo"
printf '[INFO] Rendering bundled image\n'
"$project_dir/build/linux-debug/mikumikudesu" --hidden --frames 3 --no-validation \
  --asset "$sample_image"
if command -v ffmpeg >/dev/null 2>&1; then
  printf '[INFO] Decoding generated FFmpeg audio/video sample\n'
  ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i color=c=blue:s=64x64:r=5:d=0.4 \
    -f lavfi -i sine=frequency=440:sample_rate=48000:duration=0.4 \
    -c:v ffv1 -c:a pcm_s16le "$test_dir/media.mkv"
  "$project_dir/build/linux-debug/mikumikudesu" --hidden --frames 3 --no-validation \
    --asset "$test_dir/media.mkv"
else
  printf '[WARN] ffmpeg executable is unavailable; generated media smoke test skipped\n' >&2
fi
printf '[INFO] Linux AMD checks completed\n'
