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

# The selected second contains a tone; reading audio from zero would export silence.
ffmpeg -v error -f lavfi -i 'aevalsrc=if(lt(t\,1)\,0\,0.5*sin(2*PI*440*t)):s=48000:d=2' \
  -c:a pcm_s16le "$test_directory/offset.wav"
MESA_VK_WSI_PRESENT_MODE=mailbox \
  "$binary" --export-video "$test_directory/offset.mp4" --audio-source "$test_directory/offset.wav" \
  --video-width 64 --video-height 48 --video-fps 60 --video-from-frame 30 --video-to-frame 32 --no-validation
ffmpeg -hide_banner -i "$test_directory/offset.mp4" -vn -af volumedetect -f null - 2>&1 |
  awk '/max_volume:/ { ok = $(NF-1) != "-inf" && $(NF-1) > -20 } END { exit !ok }'
