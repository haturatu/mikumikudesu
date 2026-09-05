#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
executable="$project_dir/MikuMikuDayo/MikuMikuDayo.exe"

if ! command -v wine >/dev/null 2>&1; then
  printf '[ERROR] wine is not installed\n' >&2
  exit 1
fi
python3 "$project_dir/scripts/fetch-mikumikudayo.py"
if [[ ! -f "$executable" ]]; then
  printf '[ERROR] Windows distribution executable is missing: %s\n' "$executable" >&2
  exit 1
fi

wine_prefix=$(mktemp -d -t mikumikudesu-wine.XXXXXXXX)
cleanup() {
  if [[ "$wine_prefix" == /tmp/mikumikudesu-wine.* ]]; then
    rm -rf -- "$wine_prefix"
  fi
}
trap cleanup EXIT

printf '[INFO] Running isolated Wine/vkd3d smoke test for 20 seconds\n'
set +e
WINEPREFIX="$wine_prefix" WINEDEBUG=-all timeout 20s wine "$executable"
status=$?
set -e
if [[ $status -eq 0 ]]; then
  printf '[INFO] Windows binary exited successfully under Wine\n'
elif [[ $status -eq 124 ]]; then
  printf '[INFO] Windows binary remained alive for the smoke-test interval\n'
else
  printf '[WARN] Wine smoke test exited with status %d; inspect codec/DXR/vkd3d setup\n' "$status" >&2
  exit "$status"
fi
