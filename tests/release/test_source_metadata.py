"""Source identity fixtures use real Git repositories, never the user's config."""

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "source_metadata", ROOT / "components/desk_uart/source_metadata.py"
)
metadata = importlib.util.module_from_spec(spec)
spec.loader.exec_module(metadata)


class SourceMetadataTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(
            dir=os.environ["TMPDIR"], prefix="desk-source-"
        )
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "repo"
        self.component = self.root / "components/desk_uart"
        self.component.mkdir(parents=True)
        (self.component / "project_version.json").write_text(
            '{"schema_version":1,"version":"2026.11.0"}'
        )
        (self.root / "library.json").write_text('{"name":"desk_protocol"}')
        (self.root / ".gitignore").write_text("__pycache__/\nbuild/\n")
        self.git("init")
        self.git("config", "user.name", "Fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("add", ".")
        self.git("commit", "-m", "fixture")

    def git(self, *args, cwd=None):
        env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        return subprocess.check_output(
            [
                "git",
                "-c",
                "core.hooksPath=" + os.devnull,
                "-C",
                str(cwd or self.root),
                *args,
            ],
            env=env,
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()

    def read(self, component=None):
        return metadata.source_metadata(component or self.component)

    def test_clean_dirty_ignored_and_foreign_environment(self):
        expected = self.git("rev-parse", "HEAD")
        self.assertEqual(
            self.read(), dict(version="2026.11.0", revision=expected, state="clean")
        )
        (self.component / "__pycache__").mkdir()
        (self.component / "__pycache__/test.pyc").write_bytes(b"fixture")
        self.assertEqual(self.read()["state"], "clean")
        with patch.dict(
            os.environ,
            {
                "GIT_DIR": "/nonexistent",
                "GIT_WORK_TREE": "/nonexistent",
                "GIT_COMMON_DIR": "/nonexistent",
                "GIT_INDEX_FILE": "/nonexistent",
                "GIT_CEILING_DIRECTORIES": str(self.root),
            },
        ):
            self.assertEqual(self.read()["revision"], expected)
        (self.root / "untracked").write_text("x")
        self.assertEqual(self.read()["state"], "dirty")
        (self.root / "untracked").unlink()
        (self.root / "library.json").write_text(
            '{"name":"desk_protocol", "changed":true}'
        )
        self.assertEqual(self.read()["state"], "dirty")

    def test_copied_components_foreign_repo_and_missing_metadata(self):
        dest = self.root / "config/components/desk_uart"
        shutil.copytree(self.component, dest)
        self.assertEqual(self.read(dest)["revision"], "unknown")
        (self.root / "library.json").unlink()
        self.assertEqual(self.read()["revision"], "unknown")
        self.assertEqual(self.read()["version"], "2026.11.0")
        (self.component / "project_version.json").write_text(
            '{"schema_version":true,"version":"2026.11.0"}'
        )
        self.assertEqual(self.read()["version"], "unknown")
        (self.component / "project_version.json").unlink()
        self.assertEqual(self.read()["version"], "unknown")

    def test_git_failures_never_mean_clean(self):
        for error in [
            FileNotFoundError(),
            subprocess.TimeoutExpired("git", 3),
            subprocess.CalledProcessError(128, "git", stderr="unsafe repository"),
        ]:
            with (
                self.subTest(error=type(error)),
                patch.object(metadata.subprocess, "run", side_effect=error),
            ):
                self.assertEqual(
                    self.read(),
                    dict(version="2026.11.0", revision="unknown", state="unknown"),
                )

    def test_worktree_shallow_submodule_and_annotated_tags(self):
        expected = self.git("rev-parse", "HEAD")
        worktree = Path(self.temp.name) / "worktree"
        self.git("worktree", "add", "--detach", str(worktree))
        self.assertEqual(
            self.read(worktree / "components/desk_uart")["revision"], expected
        )
        shallow = Path(self.temp.name) / "shallow"
        self.git("clone", "--depth=1", self.root.as_uri(), str(shallow))
        self.assertEqual(
            self.read(shallow / "components/desk_uart")["revision"], expected
        )
        parent = Path(self.temp.name) / "super"
        parent.mkdir()
        self.git("init", cwd=parent)
        self.git(
            "-c",
            "protocol.file.allow=always",
            "submodule",
            "add",
            str(self.root),
            "library",
            cwd=parent,
        )
        self.assertEqual(
            self.read(parent / "library/components/desk_uart")["revision"], expected
        )
        for name, args in [("light", []), ("annotated", ["-a", "-m", "fixture"])]:
            self.git("tag", *args, name)
            self.assertEqual(
                self.git("rev-parse", f"refs/tags/{name}^{{commit}}"), expected
            )


if __name__ == "__main__":
    unittest.main()
