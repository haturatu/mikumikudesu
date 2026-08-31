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
  --video-width 64 --video-height 48 --video-fps 30 --video-to-frame 2 --no-validation
test -s "$test_directory/output.mp4"
ffprobe -v error -select_streams v:0 \
    -show_entries stream=codec_type,width,height,nb_frames \
    -of csv=p=0 "$test_directory/output.mp4" | grep -Fq 'video,64,48,3'
test "$(ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 \
    "$test_directory/output.mp4")" = "0.100000"

MESA_VK_WSI_PRESENT_MODE=mailbox \
  "$binary" --export-video "$test_directory/output-60fps.mp4" --no-audio \
  --video-width 64 --video-height 48 --video-fps 60 --video-to-frame 2 --no-validation
test "$(ffprobe -v error -select_streams v:0 -show_entries stream=nb_frames \
    -of default=nw=1:nk=1 "$test_directory/output-60fps.mp4")" = "5"
