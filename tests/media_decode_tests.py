#!/usr/bin/env python3
"""Compare sequential and seeked frames with full FFmpeg RGBA references."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile


def run(*command):
    return subprocess.check_output(command, text=True)


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='dayo-media-decode-') as directory:
        root = Path(directory)
        for name, bframes, vfr, audio in [('gop60', 0, False, False),
                                         ('bframes-audio', 2, False, True),
                                         ('vfr', 2, True, False)]:
            video = root / (name + '.mp4')
            command = ['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                       'testsrc2=size=64x48:rate=30:duration=3']
            if audio:
                command += ['-f', 'lavfi', '-i', 'sine=frequency=440:duration=3']
            if vfr:
                command += ['-vf', "select='not(mod(n,3))+not(mod(n,7))'", '-fps_mode', 'vfr']
            command += ['-c:v', 'libx264', '-preset', 'fast', '-crf', '18' if bframes else '0', '-g', '60',
                        '-keyint_min', '60', '-sc_threshold', '0', '-bf', str(bframes),
                        '-x264-params', 'b-adapt=0', str(video)]
            subprocess.run(command, check=True)
            verify(binary, root, video, name, bframes, vfr)
        segments = []
        for index, size in enumerate(['64x48', '96x64']):
            segment = root / f'segment{index}.ts'
            subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                            f'testsrc2=size={size}:rate=30:duration=1', '-c:v', 'libx264',
                            '-g', '30', '-bf', '0', '-output_ts_offset', str(index),
                            '-muxdelay', '0', '-f', 'mpegts', str(segment)], check=True)
            segments.append(segment.read_bytes())
        transport = root / 'resize.ts'
        transport.write_bytes(b''.join(segments))
        video = root / 'resize.mkv'
        subprocess.run(['ffmpeg', '-v', 'error', '-i', str(transport), '-map', '0:v:0',
                        '-c', 'copy', str(video)], check=True)
        verify(binary, root, video, 'resize', 0, False, changing_dimensions=True)


def verify(binary, root, video, name, bframes, vfr, changing_dimensions=False):
    frames = json.loads(run('ffprobe', '-v', 'error', '-select_streams', 'v:0',
                            '-show_frames', '-show_entries',
                            'frame=best_effort_timestamp_time,width,height,pict_type', '-of', 'json',
                            str(video)))['frames']
    if bframes:
        assert any(f['pict_type'] == 'B' for f in frames), 'fixture must contain B frames'
    if vfr:
        gaps = {round(float(b['best_effort_timestamp_time']) -
                      float(a['best_effort_timestamp_time']), 4)
                for a, b in zip(frames, frames[1:])}
        assert len(gaps) > 1, 'fixture must have variable frame intervals'
    if changing_dimensions:
        assert len({(f['width'], f['height']) for f in frames}) > 1
    pts = root / (name + '.pts')
    pts.write_text(''.join(f"{f['best_effort_timestamp_time']} {f['width']} {f['height']}\n"
                           for f in frames))
    raw = root / (name + '.rgba')
    if not changing_dimensions:
        subprocess.run(['ffmpeg', '-v', 'error', '-i', str(video), '-map', '0:v:0',
                        '-sws_flags', 'bilinear', '-pix_fmt', 'rgba', '-fps_mode', 'passthrough',
                        '-f', 'rawvideo', str(raw)], check=True)
    subprocess.run([binary, str(video), '-' if changing_dimensions else str(raw), str(pts)], check=True)


if __name__ == '__main__':
    main()
