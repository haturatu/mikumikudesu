#!/usr/bin/env python3
"""Fetch and install the pinned MikuMikuDayo release asset."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import sys
import tempfile
import urllib.error
import urllib.request
import uuid
import zipfile


LOCK_KEYS = ("VERSION", "ARCHIVE", "URL", "SHA256")
MANIFEST_FILE = Path(__file__).resolve().parents[1] / "deps/mikumikudayo-runtime.manifest"
# Essential entry points within the packaged directories.
REQUIRED_PATHS = (
    "renderer/Preview.fxdayo",
    "renderer/Subayai.fxdayo",
    "renderer/BDPT.fxdayo",
    "licence/MikuMikuDayo.txt",
    "res/dayoicon.png",
    "particle/Smoke.dds",
)


def runtime_paths() -> tuple[str, ...]:
    paths = tuple(
        line.strip() for line in MANIFEST_FILE.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    )
    if not paths:
        raise RuntimeError("runtime manifest is empty")
    for path in paths:
        if (
            path.startswith("/")
            or "\\" in path
            or ":" in path
            or any(part in {"", ".", ".."} for part in path.rstrip("/").split("/"))
        ):
            raise RuntimeError(f"unsafe runtime manifest path: {path}")
    return paths


def log(level: str, message: str) -> None:
    stream = sys.stdout if level in {"DEBUG", "INFO"} else sys.stderr
    print(f"[{level}] {message}", file=stream)


def fail(message: str) -> None:
    log("FATAL", message)
    raise SystemExit(1)


def parse_lock(lock_file: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    try:
        lines = lock_file.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        fail(f"cannot read lock file {lock_file}: {error}")

    for line_number, line in enumerate(lines, start=1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if "=" not in stripped:
            fail(f"invalid lock file line {line_number}: expected KEY=VALUE")
        key, value = (part.strip() for part in stripped.split("=", 1))
        if key not in LOCK_KEYS:
            fail(f"unsupported lock file key {key!r} on line {line_number}")
        if key in values:
            fail(f"duplicate lock file key {key!r} on line {line_number}")
        if not value:
            fail(f"empty lock file value for {key} on line {line_number}")
        values[key] = value

    missing = [key for key in LOCK_KEYS if key not in values]
    if missing:
        fail(f"lock file is missing: {', '.join(missing)}")
    if Path(values["ARCHIVE"]).name != values["ARCHIVE"]:
        fail("ARCHIVE must be a file name without directory components")
    if "/" in values["VERSION"] or "\\" in values["VERSION"]:
        fail("VERSION must not contain path separators")
    if len(values["SHA256"]) != 64 or any(char not in "0123456789abcdefABCDEF" for char in values["SHA256"]):
        fail("SHA256 must be a 64-character hexadecimal digest")
    return values


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(url: str, destination: Path) -> None:
    temporary = destination.with_name(f".{destination.name}.{uuid.uuid4().hex}.tmp")
    request = urllib.request.Request(url, headers={"User-Agent": "mikumikudesu dependency fetcher"})
    try:
        with urllib.request.urlopen(request, timeout=60) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output, length=1024 * 1024)
        os.replace(temporary, destination)
    except (OSError, urllib.error.URLError) as error:
        temporary.unlink(missing_ok=True)
        raise RuntimeError(f"download failed: {error}") from error


def archive_for(lock: dict[str, str], cache_dir: Path) -> Path:
    archive = cache_dir / lock["ARCHIVE"]
    cache_dir.mkdir(parents=True, exist_ok=True)
    if archive.is_file():
        actual = sha256(archive)
        if actual == lock["SHA256"].lower():
            log("INFO", f"using cached {archive}")
            return archive
        log("WARN", f"cached archive checksum mismatch; replacing {archive}")
        archive.unlink()
    elif archive.exists():
        fail(f"cached archive path is not a regular file: {archive}")

    log("INFO", f"downloading {lock['URL']}")
    download(lock["URL"], archive)
    actual = sha256(archive)
    if actual != lock["SHA256"].lower():
        archive.unlink(missing_ok=True)
        fail(f"downloaded archive checksum mismatch: expected {lock['SHA256']}, got {actual}")
    log("INFO", f"verified {lock['ARCHIVE']} ({actual})")
    return archive


def safe_member_path(name: str) -> PurePosixPath:
    if not name or "\x00" in name:
        raise RuntimeError("archive contains an invalid empty or NUL path")
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts:
        raise RuntimeError(f"archive contains an unsafe path: {name}")
    return path


def extract_archive(archive: Path, staging_dir: Path) -> Path:
    seen: set[PurePosixPath] = set()
    with zipfile.ZipFile(archive) as source:
        for member in source.infolist():
            member_path = safe_member_path(member.filename)
            if member_path in seen:
                raise RuntimeError(f"archive contains duplicate path: {member.filename}")
            seen.add(member_path)
            mode = member.external_attr >> 16
            if stat.S_ISLNK(mode):
                raise RuntimeError(f"archive contains unsupported symlink: {member.filename}")
            destination = staging_dir.joinpath(*member_path.parts)
            if member.is_dir():
                destination.mkdir(parents=True, exist_ok=True)
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            with source.open(member) as input_file, destination.open("wb") as output:
                shutil.copyfileobj(input_file, output, length=1024 * 1024)

    named_root = staging_dir / "MikuMikuDayo"
    if named_root.is_dir():
        return named_root
    children = list(staging_dir.iterdir())
    directories = [child for child in children if child.is_dir()]
    if len(directories) == 1:
        return directories[0]
    return staging_dir


def validate_installation(install_dir: Path) -> None:
    missing = [
        path
        for path in runtime_paths()
        if not ((install_dir / path).is_dir() if path.endswith("/") else (install_dir / path).is_file())
    ]
    missing.extend(path for path in REQUIRED_PATHS if not (install_dir / path).is_file())
    if missing:
        raise RuntimeError(f"archive is missing required paths: {', '.join(missing)}")


def marker_contents(lock: dict[str, str]) -> str:
    return "".join(f"{key}={lock[key]}\n" for key in LOCK_KEYS)


def is_current_installation(install_dir: Path, lock: dict[str, str]) -> bool:
    marker = install_dir / ".mikumikudayo-ready"
    if not install_dir.is_dir() or not marker.is_file():
        return False
    try:
        if marker.read_text(encoding="utf-8") != marker_contents(lock):
            return False
        validate_installation(install_dir)
        return True
    except (OSError, RuntimeError):
        return False


def install_archive(archive: Path, install_dir: Path, lock: dict[str, str]) -> None:
    install_dir.parent.mkdir(parents=True, exist_ok=True)
    staging_dir = Path(tempfile.mkdtemp(prefix=f".{install_dir.name}.staging-", dir=install_dir.parent))
    backup_dir: Path | None = None
    try:
        staged_root = extract_archive(archive, staging_dir)
        validate_installation(staged_root)
        marker = staged_root / ".mikumikudayo-ready"
        marker_temporary = staged_root / f".{marker.name}.{uuid.uuid4().hex}.tmp"
        marker_temporary.write_text(marker_contents(lock), encoding="utf-8")
        os.replace(marker_temporary, marker)

        if install_dir.is_symlink():
            raise RuntimeError(f"refusing to replace symlink install path: {install_dir}")
        if install_dir.exists():
            backup_dir = install_dir.with_name(f".{install_dir.name}.backup-{uuid.uuid4().hex}")
            os.replace(install_dir, backup_dir)
        try:
            os.replace(staged_root, install_dir)
        except OSError:
            if backup_dir is not None and not install_dir.exists():
                os.replace(backup_dir, install_dir)
            raise
        if backup_dir is not None:
            shutil.rmtree(backup_dir)
    finally:
        shutil.rmtree(staging_dir, ignore_errors=True)


def fetch(lock_file: Path, install_dir: Path, cache_dir: Path) -> None:
    lock = parse_lock(lock_file)
    if is_current_installation(install_dir, lock):
        log("INFO", f"MikuMikuDayo {lock['VERSION']} is ready at {install_dir}")
        return
    archive = archive_for(lock, cache_dir)
    log("INFO", f"installing MikuMikuDayo {lock['VERSION']} into {install_dir}")
    install_archive(archive, install_dir, lock)
    log("INFO", f"MikuMikuDayo {lock['VERSION']} is ready")


def arguments() -> argparse.Namespace:
    project_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock-file", type=Path, default=project_root / "deps/mikumikudayo.lock")
    parser.add_argument("--install-dir", type=Path, default=project_root / "MikuMikuDayo")
    parser.add_argument("--cache-dir", type=Path, default=project_root / ".cache/mikumikudayo")
    return parser.parse_args()


def main() -> int:
    options = arguments()
    try:
        fetch(options.lock_file, options.install_dir, options.cache_dir)
    except SystemExit:
        raise
    except (OSError, RuntimeError, ValueError, zipfile.BadZipFile) as error:
        fail(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
