#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

printf '[INFO] Checking Vulkan/RADV environment\n'
if ! command -v vulkaninfo >/dev/null 2>&1; then
  printf '[ERROR] vulkaninfo is required (install vulkan-tools)\n' >&2
  exit 1
fi
vulkaninfo --summary

printf '[INFO] Configuring linux-debug preset\n'
cmake --preset linux-debug -S "$project_dir"
printf '[INFO] Building mikumikudesu\n'
cmake --build --preset linux-debug
printf '[INFO] Running CTest suite\n'
ctest --preset linux-debug
printf '[INFO] Probing renderer features\n'
"$project_dir/build/linux-debug/mikumikudesu" --probe --hidden --renderer bdpt
printf '[INFO] Rendering bundled PMX smoke test\n'
"$project_dir/build/linux-debug/mikumikudesu" --hidden --frames 10 \
  --asset "$project_dir/MikuMikuDayo/sample/deformTutorial.pmx"
printf '[INFO] Linux AMD checks completed\n'

