#!/usr/bin/env bash
set -euo pipefail

binary=$1
test_directory=$(mktemp -d)
cleanup() {
  "${CMAKE_COMMAND:-cmake}" -E remove_directory "$test_directory"
}
trap cleanup EXIT

ffmpeg -v error -f lavfi -i "sine=frequency=440:sample_rate=8000" -t 1 \
  -ac 1 -c:a pcm_s16le "$test_directory/input.wav"
"$binary" --audio-source "$test_directory/input.wav" --export-m4a "$test_directory/output.m4a"
test -s "$test_directory/output.m4a"
ffprobe -v error -select_streams a:0 -show_entries stream=codec_name \
  -of csv=p=0 "$test_directory/output.m4a" | grep -Fxq aac

"$binary" --audio-source "$test_directory/input.wav" --export-m4a "$test_directory/partial.m4a" \
  --audio-to 0.123
ffprobe -v error -select_streams a:0 -show_entries stream=duration \
  -of csv=p=0 "$test_directory/partial.m4a" |
  awk '{ delta = $1 - 0.123; if (delta < 0) delta = -delta; ok = delta < 0.001 } END { exit !ok }'
