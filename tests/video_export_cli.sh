#!/usr/bin/env bash
set -euo pipefail

binary=$1
test_directory=$(mktemp -d)
cleanup() {
    "${CMAKE_COMMAND:-cmake}" -E remove_directory "$test_directory"
}
trap cleanup EXIT

MESA_VK_WSI_PRESENT_MODE=mailbox \
  "$binary" --export-video "$test_directory/output.mp4" --no-audio \
  --video-width 64 --video-height 48 --video-fps 10 --video-to-frame 2 --no-validation
test -s "$test_directory/output.mp4"
ffprobe -v error -select_streams v:0 \
    -show_entries stream=codec_type,width,height,nb_frames \
    -of csv=p=0 "$test_directory/output.mp4" | grep -Fq 'video,64,48'
