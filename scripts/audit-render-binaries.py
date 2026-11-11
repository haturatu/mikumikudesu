#!/usr/bin/env python3
"""Collect static ELF/SPIR-V evidence; instruction counts are not timings."""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess


FUNCTIONS = (
    'dayo::app::Application::refreshAnimatedMesh(',
    'dayo::graphics::VulkanDevice::renderFrame(',
    'dayo::graphics::VulkanDevice::recordPreviewDeform(',
    'dayo::graphics::VulkanDevice::collectRenderedImage(',
    'dayo::core::MediaFile::decodeVideoFrame(',
    'mmd::MmdAnimator::evaluateInto(',
    'mmd::(anonymous namespace)::rebuildBonePoses(',
    'mmd::(anonymous namespace)::solveIk(',
)
SHADERS = (
    'preview_tonemap.PS.spv',
    'preview.PS.spv',
    'preview_deform.PreviewDeform.spv',
    'preview_dof.DofFilter.spv',
)


def run(*command):
    return subprocess.check_output(command, text=True)


def identity(path):
    return {'name': path.name, 'bytes': path.stat().st_size,
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def shader_evidence(path, output):
    assembly = run('spirv-dis', str(path))
    (output / (path.name + '.txt')).write_text(assembly)
    opcodes = Counter(re.findall(r'\b(Op\w+)\b', assembly))
    extended = Counter(re.findall(r'OpExtInst\s+%\S+\s+%\S+\s+(\w+)', assembly))
    return {**identity(path), 'static_instructions': sum(opcodes.values()),
            'opcodes': dict(sorted(opcodes.items())),
            'extended_instructions': dict(sorted(extended.items()))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--shaders', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    # Raw mangled names let objdump select one exact symbol, including clones.
    raw = run('nm', '-S', '--defined-only', str(args.binary))
    lines = [line.split(maxsplit=3) for line in raw.splitlines()]
    symbols = [line for line in lines if len(line) == 4 and line[2] in ('t', 'T')]
    demangled = subprocess.check_output(
        ['c++filt'], input='\n'.join(line[3] for line in symbols) + '\n', text=True).splitlines()
    functions = []
    for symbol, name in zip(symbols, demangled, strict=True):
        if not name.startswith(FUNCTIONS):
            continue
        address, size, _, mangled = symbol
        assembly = run('objdump', '-d', '--no-show-raw-insn', '--disassemble=' + mangled,
                       str(args.binary))
        filename = f'function-{len(functions):02d}.txt'
        (args.output / filename).write_text(assembly)
        mnemonics = Counter(re.findall(r'^\s*[0-9a-f]+:\s+(\S+)', assembly, re.MULTILINE))
        functions.append({'name': name, 'address_hex': address, 'bytes': int(size, 16),
                          'disassembly': filename, 'static_mnemonics': dict(sorted(mnemonics.items()))})
    for name, command in {
        'elf.txt': ('readelf', '-hSWn', str(args.binary)),
        'compiler-comment.txt': ('readelf', '-p', '.comment', str(args.binary)),
    }.items():
        (args.output / name).write_text(run(*command))
    result = {
        'note': 'Static counts include mutually exclusive branches; they are not executed counts or GPU ISA.',
        'source_head_at_collection': run('git', 'rev-parse', 'HEAD').strip(),
        'source_status_at_collection': run('git', 'status', '--short', '--untracked-files=no').splitlines(),
        'binary': identity(args.binary),
        'tools': {tool: run(tool, '--version').splitlines()[0]
                  for tool in ('objdump', 'nm', 'spirv-dis')},
        'functions': functions,
        'shaders': [shader_evidence(args.shaders / name, args.output) for name in SHADERS],
    }
    (args.output / 'static.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(args.output / 'static.json')


if __name__ == '__main__':
    main()
