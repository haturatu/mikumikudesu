"""Dependency installer regressions; no network or upstream binary required."""
import contextlib
import importlib.util
import io
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location(
    "fetcher", Path(__file__).resolve().parents[1] / "scripts/fetch-mikumikudayo.py"
)
fetcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetcher)


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.install = self.root / "installed"
        self.lock = dict(VERSION="test130", ARCHIVE="test.zip", URL="unused", SHA256="a" * 64)

    def archive(self, missing=None):
        archive = self.root / "test.zip"
        with zipfile.ZipFile(archive, "w") as output:
            for path in fetcher.runtime_paths():
                if path != missing:
                    output.writestr("MikuMikuDayo/" + path, "" if path.endswith("/") else "fixture")
            for path in fetcher.REQUIRED_PATHS:
                if missing is None or not path.startswith(missing):
                    output.writestr("MikuMikuDayo/" + path, "fixture")
        return archive

    def test_manifest_install_and_stale_detection(self):
        fetcher.install_archive(self.archive(), self.install, self.lock)
        self.assertTrue(fetcher.is_current_installation(self.install, self.lock))
        (self.install / "postprocess").rmdir()
        self.assertFalse(fetcher.is_current_installation(self.install, self.lock))

    def test_failed_upgrade_preserves_previous_install(self):
        fetcher.install_archive(self.archive(), self.install, self.lock)
        sentinel = self.install / "keep.txt"
        sentinel.write_text("previous installation")
        with self.assertRaisesRegex(RuntimeError, "postprocess/"):
            fetcher.install_archive(self.archive(missing="postprocess/"), self.install, self.lock)
        self.assertEqual(sentinel.read_text(), "previous installation")
        self.assertTrue(fetcher.is_current_installation(self.install, self.lock))

    def test_version_marker_invalidates_old_release(self):
        fetcher.install_archive(self.archive(), self.install, self.lock)
        self.assertFalse(fetcher.is_current_installation(self.install, {**self.lock, "VERSION": "test120"}))

    def test_archive_traversal_rejected(self):
        archive = self.root / "unsafe.zip"
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr("../escaped", "bad")
        with self.assertRaisesRegex(RuntimeError, "unsafe path"):
            fetcher.install_archive(archive, self.install, self.lock)
        self.assertFalse((self.root / "escaped").exists())

    def test_cmake_packages_complete_runtime_directories(self):
        fetcher.install_archive(self.archive(), self.install, self.lock)
        # Non-shader resources must survive install, along with every licence.
        extras = (
            "postprocess/fog/noise.dds",
            "sample/cloning_sample/model.pmx",
            "licence/third-party/LICENSE",
            "renderer/shared/import.libsonnet",
        )
        for name in extras:
            path = self.install / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"runtime asset")
        project = self.root / "project"
        project.mkdir()
        (project / "MikuMikuDayo").symlink_to(self.install, target_is_directory=True)
        source = Path(__file__).resolve().parents[1]
        shutil.copytree(source / "deps", project / "deps")
        shutil.copytree(source / "cmake", project / "cmake")
        (project / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.25)\n'
            'project(package_fixture NONE)\ninclude(cmake/InstallMikuMikuDayo.cmake)\n'
        )
        build, prefix = self.root / "build", self.root / "prefix"
        subprocess.run(["cmake", "-S", str(project), "-B", str(build)], check=True, capture_output=True)
        subprocess.run(["cmake", "--install", str(build), "--prefix", str(prefix)],
                       check=True, capture_output=True)
        installed = prefix / "share/mikumikudesu"
        for path in self.install.rglob("*"):
            if path.is_file() and path.name != ".mikumikudayo-ready":
                self.assertEqual((installed / path.relative_to(self.install)).read_bytes(), path.read_bytes())
        self.assertEqual((installed / "mikumikudayo.lock").read_bytes(),
                         (source / "deps/mikumikudayo.lock").read_bytes())
        shutil.rmtree(self.install / "postprocess")
        failed = subprocess.run(["cmake", "--install", str(build), "--prefix", str(prefix)],
                                capture_output=True)
        self.assertNotEqual(failed.returncode, 0)

    def test_log_streams(self):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            fetcher.log("INFO", "ready")
            fetcher.log("WARN", "missing")
            with self.assertRaises(SystemExit) as failure:
                fetcher.fail("failed")
        self.assertEqual(failure.exception.code, 1)
        self.assertEqual(out.getvalue(), "[INFO] ready\n")
        self.assertEqual(err.getvalue(), "[WARN] missing\n[FATAL] failed\n")


if __name__ == "__main__":
    unittest.main()
