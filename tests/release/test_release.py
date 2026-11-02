"""Check release packaging and source guards with isolated Git fixtures."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import release  # noqa: E402 - Load tools after the sys.path bootstrap.


class ReleaseTest(unittest.TestCase):
    """Verify deterministic assets and their binding to an eligible source revision."""

    def setUp(self):
        """Copy release inputs into a temporary Git repository with a fixture commit."""
        self.temp = tempfile.TemporaryDirectory(
            dir=os.environ["TMPDIR"], prefix="desk-release-"
        )
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "repo"
        paths = {
            release.VERSION_FILE,
            "metadata/firmware-catalog.json",
            "requirements-dev.txt",
            "CHANGELOG.md",
            *release.YAMLS.values(),
            "tools/ci.py",
            "tools/check_docs.py",
            "requirements-ci.txt",
            "tools/release.py",
            "tools/release_github.py",
            ".github/workflows/release.yml",
            ".github/workflows/test.yml",
            "tests/esphome/run_all.py",
        }
        for path in paths:
            dest = self.root / path
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / path, dest)
        self.git("init")
        self.git("config", "user.name", "Fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("add", ".")
        self.git("commit", "-m", "fixture")
        self.sha = self.git("rev-parse", "HEAD")
        self.version = json.loads((self.root / release.VERSION_FILE).read_text())[
            "version"
        ]

    def git(self, *args):
        """Run Git in the fixture without inherited Git settings or repository hooks."""
        env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        return subprocess.check_output(
            ["git", "-c", "core.hooksPath=" + os.devnull, "-C", str(self.root), *args],
            env=env,
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()

    def package(self):
        """Write the fixture release assets into a new temporary package directory."""
        directory = Path(self.temp.name) / "package"
        directory.mkdir()
        for name, data in release.package_bytes(
            self.root, self.version, self.sha
        ).items():
            (directory / name).write_bytes(data)
        return directory

    def test_deterministic_package_and_refs(self):
        """Produce repeatable assets with tagged sources and flat package includes."""
        first = release.package_bytes(self.root, self.version, self.sha)
        self.assertEqual(
            first, release.package_bytes(self.root, self.version, self.sha)
        )
        self.assertEqual(set(first), release.ASSETS)
        directory = self.package()
        release.verify_package(directory, self.version, self.sha)
        source = f"source: github://{release.REPOSITORY}@v{self.version}"
        for name in [
            "stamp-c3.yaml",
            "esp32.yaml",
            "existing-device.yaml",
            "firmware-export.yaml",
            "firmware-update.yaml",
        ]:
            self.assertIn(source, first[name].decode())
            self.assertNotIn("type: local", first[name].decode())
        for name in release.YAMLS:
            self.assertNotIn("!include packages/", first[name].decode())
        self.assertNotIn("validation-report.json", first["SHA256SUMS"].decode())
        self.assertIn("https://github.com/", first["release-notes.md"].decode())

    def test_next_version_replaces_previous_stable_source(self):
        """Replace previous stable component sources with the new release tag."""
        old_version = self.version
        year, month, sequence = old_version.split(".")
        self.version = f"{year}.{month}.{int(sequence) + 1}"
        (self.root / release.VERSION_FILE).write_text(
            json.dumps({"schema_version": 1, "version": self.version})
        )
        changelog = self.root / "CHANGELOG.md"
        changelog.write_text(
            changelog.read_text().replace(
                "## Unreleased", "## Unreleased\n\n## " + self.version, 1
            )
        )
        snippet = self.root / "examples/esphome/existing-device.yaml"
        snippet.write_text(snippet.read_text().replace("@main", "@v" + old_version))
        self.assertIn("@v" + old_version, snippet.read_text())
        self.git("add", ".")
        self.git("commit", "-m", "next version fixture")
        self.sha = self.git("rev-parse", "HEAD")
        package = release.package_bytes(self.root, self.version, self.sha)
        for name in (
            "stamp-c3.yaml",
            "esp32.yaml",
            "existing-device.yaml",
            "firmware-export.yaml",
            "firmware-update.yaml",
        ):
            self.assertIn("@v" + self.version, package[name].decode())
            self.assertNotIn("@v" + old_version, package[name].decode())

    def test_source_guards(self):
        """Reject candidates with invalid metadata, identity or dirty source."""
        for version, sha in [
            ("2026.01.0", self.sha),
            ("2026.13.0", self.sha),
            (self.version, "0" * 40),
            ("2026.11.9", self.sha),
        ]:
            with self.subTest(version=version, sha=sha), self.assertRaises(ValueError):
                release.validate(self.root, version, sha)
        (self.root / "untracked").write_text("x")
        with self.assertRaises(ValueError):
            release.validate(self.root, self.version, self.sha)
        (self.root / "untracked").unlink()
        (self.root / release.VERSION_FILE).write_text("{}")
        with self.assertRaises(ValueError):
            release.validate(self.root, self.version, self.sha)

    def test_esphome_resolves_the_annotated_tag(self):
        """Resolve an annotated tag through ESPHome's actual Git helper."""
        from unittest.mock import patch

        from esphome import git as esphome_git

        self.git("tag", "-a", "v" + self.version, "-m", "release fixture")
        bare = Path(self.temp.name) / "remote.git"
        self.git("clone", "--bare", str(self.root), str(bare))
        destination = Path(self.temp.name) / "esphome-clone"
        # Keep the production clone/fetch/reset path; isolate only its destination.
        with patch.object(
            esphome_git, "_compute_destination_path", return_value=destination
        ):
            checkout, _ = esphome_git.clone_or_update(
                url=bare.as_uri(), ref="v" + self.version, refresh=None, domain="test"
            )
        self.assertEqual(release.text(checkout, "rev-parse", "HEAD"), self.sha)

    def test_tag_peeling_and_wrong_commit(self):
        """Require an annotated release tag that resolves to the requested commit."""
        with self.assertRaises(subprocess.SubprocessError):
            release.verify_tag(self.root, self.version, self.sha)
        self.git("tag", "v" + self.version)
        with self.assertRaises(ValueError):
            release.verify_tag(self.root, self.version, self.sha)
        self.git("tag", "-d", "v" + self.version)
        self.git("tag", "-a", "v" + self.version, "-m", "release fixture")
        release.verify_tag(self.root, self.version, self.sha)
        self.assertNotEqual(
            self.git("rev-parse", "refs/tags/v" + self.version), self.sha
        )
        with self.assertRaises(ValueError):
            release.verify_tag(self.root, self.version, "0" * 40)

    def test_asset_allowlist_and_tampering(self):
        """Reject extra release assets and changes to packaged content."""
        directory = self.package()
        (directory / "secret.bin").write_bytes(b"not allowed")
        with self.assertRaises(ValueError):
            release.verify_package(directory, self.version, self.sha)
        (directory / "secret.bin").unlink()
        (directory / "stamp-c3.yaml").write_text("changed")
        with self.assertRaises(ValueError):
            release.verify_package(directory, self.version, self.sha)

    def test_ci_helpers_are_bound_to_the_release_candidate(self):
        """Reject later changes to selection, documentation checks or their dependencies."""
        for name in ("tools/ci.py", "tools/check_docs.py", "requirements-ci.txt"):
            for operation in ("modify", "delete"):
                with self.subTest(path=name, operation=operation):
                    path = self.root / name
                    original = path.read_bytes()
                    if operation == "modify":
                        path.write_bytes(original + b"\n# changed\n")
                    else:
                        path.unlink()
                    self.git("add", name)
                    self.git("commit", "-m", "change CI infrastructure")
                    with self.assertRaises(ValueError):
                        release.infrastructure(
                            self.root, self.sha, self.git("rev-parse", "HEAD")
                        )
                    path.write_bytes(original)
                    self.git("add", name)
                    self.git("commit", "-m", "restore fixture")
                    release.infrastructure(
                        self.root, self.sha, self.git("rev-parse", "HEAD")
                    )

    def test_infrastructure_only_allows_unrelated_main_changes(self):
        """Allow unrelated main changes while rejecting release-infrastructure drift."""
        release.infrastructure(self.root, self.sha, self.sha)
        (self.root / "unrelated.txt").write_text("docs")
        self.git("add", ".")
        self.git("commit", "-m", "unrelated")
        release.infrastructure(self.root, self.sha, self.git("rev-parse", "HEAD"))
        with (self.root / "tools/release_github.py").open("a") as f:
            f.write("\n# changed\n")
        self.git("add", ".")
        self.git("commit", "-m", "infrastructure")
        with self.assertRaises(ValueError):
            release.infrastructure(self.root, self.sha, self.git("rev-parse", "HEAD"))


if __name__ == "__main__":
    unittest.main()
