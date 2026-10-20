"""Build and measure the same Preview source sequentially, without shared object files."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import statistics
import re
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2])
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--deps", type=Path, required=True)
parser.add_argument("--model", type=Path, required=True)
parser.add_argument("--motion", type=Path, required=True)
parser.add_argument("--jobs", type=int, default=3)
parser.add_argument(
    "--resume",
    action="store_true",
    help="reuse completed variants with unchanged production sources",
)
args = parser.parse_args()
source, output, deps = args.source.resolve(), args.output.resolve(), args.deps.resolve()
if args.jobs < 1:
    parser.error("jobs must be positive")
for path in [args.model, args.motion]:
    if not path.is_file():
        parser.error(f"missing input: {path}")
subprocess.run(["git", "diff", "--quiet"], cwd=source, check=True)
subprocess.run(["git", "diff", "--cached", "--quiet"], cwd=source, check=True)
profiles = output / "profiles"
if output.exists() and not args.resume:
    parser.error("use a new output directory for a clean build comparison")
if args.resume:
    result = json.loads((output / "results.json").read_text())
    # CMake/measurement corrections may change, while production code and
    # shaders must match the already completed variants.
    subprocess.run(
        [
            "git",
            "diff",
            "--quiet",
            result["source_commit"],
            "--",
            "src",
            "shaders",
            "external/libmmd",
        ],
        cwd=source,
        check=True,
    )
    for entry in result["variants"]:
        if entry["status"] != "complete":
            build = Path(entry["build_dir"])
            if build.resolve().parent != output or (build / "mikumikudesu").exists():
                parser.error("incomplete executable exists; use a new output directory")
            if build.exists():
                shutil.rmtree(build)
    result["variants"] = [
        entry for entry in result["variants"] if entry["status"] == "complete"
    ]
    result.setdefault("configuration_commits", []).append(
        subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=source, text=True
        ).strip()
    )
else:
    output.mkdir(parents=True)
    result = {
        "source_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=source, text=True
        ).strip(),
        "libmmd_commit": subprocess.check_output(
            ["git", "-C", "external/libmmd", "rev-parse", "HEAD"], cwd=source, text=True
        ).strip(),
        "jobs": args.jobs,
        "variants": [],
    }


def save():
    (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")


def run(command, log, environment=None):
    start = time.perf_counter()
    with log.open("w") as stream:
        subprocess.run(
            command,
            cwd=source,
            stdout=stream,
            stderr=subprocess.STDOUT,
            env=environment,
            check=True,
        )
    return time.perf_counter() - start


def measure_loads(entry):
    build = Path(entry["build_dir"])
    name = entry["name"]
    run(
        ["cmake", "-S", str(source), "-B", str(build), "-DDAYO_BUILD_BENCHMARKS=ON"],
        output / f"{name}-load-configure.log",
    )
    entry["load_tool_build_seconds"] = run(
        [
            "cmake",
            "--build",
            str(build),
            "--target",
            "dayo_model_load_bench",
            "-j",
            str(args.jobs),
        ],
        output / f"{name}-load-build.log",
    )
    log = output / f"{name}-load.log"
    run(
        [
            str(build / "dayo_model_load_bench"),
            str(args.model.resolve()),
            str(args.motion.resolve()),
        ],
        log,
    )
    line = next(
        line
        for line in reversed(log.read_text().splitlines())
        if line.startswith("model_ms=")
    )
    entry["cpu_load_ms"] = {
        key: float(value)
        for key, value in re.findall(r"(model_ms|motion_ms)=([0-9.]+)", line)
    }
    environment = dict(
        os.environ, XDG_CONFIG_HOME=str(output / f"{name}-startup-config")
    )
    base = [
        str(build / "mikumikudesu"),
        "--hidden",
        "--no-validation",
        "--renderer",
        "preview",
        "--present",
        "immediate",
        "--preview-quality",
        "apu",
        "--frames",
        "1",
    ]
    for label, assets in [
        ("startup", []),
        (
            "startup_with_assets",
            [
                "--asset",
                str(args.model.resolve()),
                "--asset",
                str(args.motion.resolve()),
            ],
        ),
    ]:
        times = []
        for repeat in range(4):
            elapsed = run(
                base + assets, output / f"{name}-{label}-{repeat}.log", environment
            )
            if repeat:
                times.append(elapsed * 1000)
        entry[label + "_ms"] = statistics.median(times)


variants = [
    ("gcc-release", "linux-release", None),
    ("gcc-native", "linux-release-native", "-march=native"),
    ("clang-release", "linux-clang-release", None),
    ("clang-thinlto", "linux-clang-thinlto", "-flto=thin"),
    ("clang-pgo-generate", "linux-pgo-generate", "-fprofile-instr-generate"),
    (
        "clang-pgo-use",
        "linux-pgo-use",
        f"-fprofile-instr-use={profiles}/default.profdata",
    ),
]
reference = None
for name, preset, flag in variants:
    completed = next(
        (entry for entry in result["variants"] if entry["name"] == name), None
    )
    if completed:
        if "cpu_load_ms" not in completed:
            print(f"{name}: measuring startup and CPU loading", flush=True)
            measure_loads(completed)
            save()
        if reference is None:
            reference = subprocess.check_output(
                [
                    "ffmpeg",
                    "-v",
                    "error",
                    "-i",
                    str(output / f"{name}.mp4"),
                    "-f",
                    "rawvideo",
                    "-pix_fmt",
                    "rgb24",
                    "-",
                ]
            )
        print(f"{name}: reusing complete measurement", flush=True)
        continue
    print(f"{name}: configuring", flush=True)
    build = output / name
    entry = {
        "name": name,
        "preset": preset,
        "build_dir": str(build),
        "status": "configuring",
    }
    result["variants"].append(entry)
    save()
    configure = [
        "cmake",
        "--preset",
        preset,
        "-S",
        str(source),
        "-B",
        str(build),
        "-DBUILD_TESTING=OFF",
        "-DDAYO_BUILD_BENCHMARKS=ON",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DCMAKE_CXX_COMPILER_LAUNCHER=",
        f"-DDAYO_PGO_PROFILE_DIR={profiles}",
    ]
    for dependency in ["imgui", "stb", "vulkan_memory_allocator", "jsonnet_source"]:
        configure.append(
            f"-DFETCHCONTENT_SOURCE_DIR_{dependency.upper()}={deps}/{dependency}-src"
        )
    entry["configure_seconds"] = run(configure, output / f"{name}-configure.log")
    commands = json.loads((build / "compile_commands.json").read_text())
    checks = []
    for suffix in [
        "/formats/pmx_reader.cpp",
        "/animation/animation.cpp",
        "/physics/physics.cpp",
        "/core/image.cpp",
    ]:
        command = next(c["command"] for c in commands if c["file"].endswith(suffix))
        options = shlex.split(command)
        if flag and not any(
            option == flag or option.startswith(flag + "=") for option in options
        ):
            raise RuntimeError(f"{name}: missing {flag} in {suffix}")
        if flag is None and any(
            o.startswith(("-march=native", "-flto=", "-fprofile-")) for o in options
        ):
            raise RuntimeError(f"{name}: unexpected opt-in flags in {suffix}")
        checks.append({"source": suffix, "command": command})
    entry["checked_compile_commands"] = checks
    print(f"{name}: building", flush=True)
    entry["status"] = "building"
    save()
    entry["build_seconds"] = run(
        [
            "cmake",
            "--build",
            str(build),
            "--target",
            "mikumikudesu",
            "-j",
            str(args.jobs),
        ],
        output / f"{name}-build.log",
    )
    binary = build / "mikumikudesu"
    entry["binary_bytes"] = binary.stat().st_size
    if name == "clang-pgo-generate":
        print(f"{name}: training and merging profiles", flush=True)
        run(
            [
                "python",
                str(source / "scripts/benchmark-preview.py"),
                "--binary",
                str(binary),
                "--model",
                str(args.model.resolve()),
                "--motion",
                str(args.motion.resolve()),
                "--output",
                str(output / "training"),
                "--models",
                "1",
                "3",
                "--runs",
                "1",
                "--frames",
                "600",
            ],
            output / "training.log",
        )
    else:
        print(f"{name}: measuring 1/3/5 models", flush=True)
        run(
            [
                "python",
                str(source / "scripts/benchmark-preview.py"),
                "--binary",
                str(binary),
                "--model",
                str(args.model.resolve()),
                "--motion",
                str(args.motion.resolve()),
                "--output",
                str(output / f"{name}-preview"),
                "--models",
                "1",
                "3",
                "5",
                "--runs",
                "3",
            ],
            output / f"{name}-preview.log",
        )
        entry["preview"] = json.loads(
            (output / f"{name}-preview/results.json").read_text()
        )
    # Fixed-time output also exercises the offscreen and encoder paths during PGO training.
    environment = dict(os.environ, XDG_CONFIG_HOME=str(output / f"{name}-config"))
    video = output / f"{name}.mp4"
    run(
        [
            str(binary),
            "--hidden",
            "--no-validation",
            "--renderer",
            "preview",
            "--preview-quality",
            "apu",
            "--asset",
            str(args.model.resolve()),
            "--asset",
            str(args.motion.resolve()),
            "--export-video",
            str(video),
            "--video-width",
            "320",
            "--video-height",
            "320",
            "--video-fps",
            "30",
            "--video-from-frame",
            "30",
            "--video-to-frame",
            "31",
            "--no-audio",
            "--overwrite",
        ],
        output / f"{name}-export.log",
        environment,
    )
    pixels = subprocess.check_output(
        [
            "ffmpeg",
            "-v",
            "error",
            "-i",
            str(video),
            "-f",
            "rawvideo",
            "-pix_fmt",
            "rgb24",
            "-",
        ]
    )
    entry["rgb_sha256"] = hashlib.sha256(pixels).hexdigest()
    if reference is None:
        reference = pixels
    if len(pixels) != len(reference):
        raise RuntimeError(f"{name}: output frame count or dimensions changed")
    errors = [abs(a - b) for a, b in zip(reference, pixels)]
    entry["rgb_mean_error"] = sum(errors) / len(errors)
    entry["rgb_max_error"] = max(errors)
    entry["rgb_changed_channels"] = sum(e != 0 for e in errors)
    print(f"{name}: measuring startup and CPU loading", flush=True)
    measure_loads(entry)
    if name == "clang-pgo-generate":
        run(
            ["cmake", "--build", str(build), "--target", "dayo_pgo_merge"],
            output / "profile-merge.log",
        )
        entry["raw_profiles"] = len(list(profiles.glob("*.profraw")))
    entry["status"] = "complete"
    save()
    print(
        f"{name}: complete ({entry['build_seconds']:.1f}s build, {entry['binary_bytes']} bytes)",
        flush=True,
    )
