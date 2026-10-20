#!/usr/bin/env python3
"""Measure real Preview playback; no GPU/CPU sums are interpreted as frame time."""

import argparse
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--motion', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--frames', type=int, default=300)
    parser.add_argument('--models', type=int, nargs='+', default=[1, 3, 5])
    parser.add_argument('--quality', choices=['auto', 'apu', 'high'], default='apu')
    args = parser.parse_args()
    if min(args.runs, args.frames, *args.models) <= 0:
        parser.error('counts must be positive')
    for path in [args.binary, args.model, args.motion]:
        if not path.is_file():
            parser.error(f'file does not exist: {path}')
    args.output.mkdir(parents=True, exist_ok=True)
    config = args.output.resolve() / 'config'
    config.mkdir(exist_ok=True)
    environment = dict(os.environ, XDG_CONFIG_HOME=str(config))
    result = {'binary': str(args.binary.resolve()), 'binary_bytes': args.binary.stat().st_size,
              'quality': args.quality, 'frames': args.frames, 'runs': []}
    for models in args.models:
        base = [str(args.binary.resolve()), '--hidden', '--no-validation', '--renderer', 'preview',
                '--present', 'immediate', '--preview-quality', args.quality]
        for _ in range(models):
            base += ['--asset', str(args.model.resolve()), '--asset', str(args.motion.resolve())]
        # Warm model/texture/pipeline/OS caches before the measured repetitions.
        subprocess.run(base + ['--frames', '60'], env=environment, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=True)
        for run in range(args.runs):
            command = base + ['--frames', str(args.frames)]
            started = time.perf_counter()
            process = subprocess.run(command, env=environment, capture_output=True, text=True, check=True)
            wall = time.perf_counter() - started
            prefix = args.output / f'{models}-models-{run + 1}'
            prefix.with_suffix('.log').write_text(process.stdout + process.stderr)
            profile = next(line for line in reversed(process.stdout.splitlines()) if 'profile: ' in line)
            passes = next(line for line in reversed(process.stdout.splitlines()) if 'gpu passes: ' in line)
            metrics = {key: float(value) for key, value in re.findall(r'([\w/]+)=([\d.]+)ms', profile)}
            gpu = {key: float(value) for key, value in re.findall(r'([\w/]+)=([\d.]+)ms', passes)}
            record = {'models': models, 'run': run + 1, 'wall_seconds_including_startup': wall,
                      'cpu_and_frame_ms': metrics, 'gpu_pass_ms': gpu, 'command': command}
            result['runs'].append(record)
            (args.output / 'results.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
        rows = [row for row in result['runs'] if row['models'] == models]
        print(json.dumps({'models': models, 'median_ms': {
            key: statistics.median(row['cpu_and_frame_ms'][key] for row in rows)
            for key in rows[0]['cpu_and_frame_ms']
        }}, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
