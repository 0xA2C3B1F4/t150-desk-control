"""Protect asset-formatting scope and the pinned toolchain."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import format_assets  # noqa: E402 - Import checkout tooling after the path bootstrap.
import install_ci_node  # noqa: E402


class AssetStyleTest(unittest.TestCase):
    """Keep generated files out of formatting and reject tool-version drift."""

    def setUp(self):
        """Create an isolated source tree under the configured scratch directory."""
        self.temp = tempfile.TemporaryDirectory(dir=os.environ["TMPDIR"])
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()

    def test_scope_excludes_generated_and_untracked_files(self):
        """Only approved, tracked web sources may reach the formatter."""
        name = "components/desk_firmware_update/ui.html"
        path = self.root / name
        path.parent.mkdir(parents=True)
        path.write_text("<html></html>\n")
        protected = [
            "components/desk_firmware_update/ui.h",
            "build/page.html",
            "third_party/x.js",
            "AGENTS.md",
        ]
        with patch(
            "format_assets.subprocess.check_output",
            return_value="\0".join([name, *protected]) + "\0",
        ):
            self.assertEqual(format_assets.selected_files(self.root, ["web"]), [name])

    def test_symlinked_source_is_rejected(self):
        """A tracked source symlink cannot redirect a formatter write."""
        name = "components/desk_firmware_update/ui.html"
        path = self.root / name
        path.parent.mkdir(parents=True)
        target = self.root / "other.html"
        target.write_text("<html></html>\n")
        path.symlink_to(target)
        with patch("format_assets.subprocess.check_output", return_value=name + "\0"):
            with self.assertRaises(SystemExit):
                format_assets.selected_files(self.root, ["web"])

    def test_version_requires_an_exact_match(self):
        """A different patch version or trailing version text must fail preflight."""
        for actual in ["v26.13.1", "v26.13.0 modified", "26.13.0"]:
            with patch("format_assets.subprocess.check_output", return_value=actual):
                with self.assertRaises(SystemExit):
                    format_assets.require_version(["node"], "v26.13.0")
        with patch("format_assets.subprocess.check_output", return_value="v26.13.0\n"):
            format_assets.require_version(["node"], "v26.13.0")

    def test_manifests_and_installer_agree(self):
        """Node and Prettier pins must agree with their installer and npm lockfile."""
        package = json.loads((ROOT / "tools/style/package.json").read_text())
        lock = json.loads((ROOT / "tools/style/package-lock.json").read_text())
        node = (ROOT / ".node-version").read_text().strip()
        self.assertEqual(node, install_ci_node.VERSION)
        self.assertEqual(node, package["engines"]["node"])
        self.assertTrue(package["private"])
        prettier = package["devDependencies"]["prettier"]
        self.assertEqual(prettier, lock["packages"]["node_modules/prettier"]["version"])
        self.assertEqual(prettier, lock["packages"][""]["devDependencies"]["prettier"])

    def test_formatter_errors_are_not_hidden(self):
        """A failed check must propagate to the invoking CI job."""
        version = json.loads((ROOT / "tools/style/package.json").read_text())[
            "devDependencies"
        ]["prettier"]
        with patch("format_assets.subprocess.check_output", return_value=version):
            with patch(
                "format_assets.subprocess.run",
                side_effect=subprocess.CalledProcessError(1, "prettier"),
            ):
                with self.assertRaises(subprocess.CalledProcessError):
                    format_assets.run_prettier(
                        Path("node"), Path("prettier"), ["page.html"], "--check"
                    )


if __name__ == "__main__":
    unittest.main()
