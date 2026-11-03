"""Check formatter scope and preflight guards with isolated Git repositories."""

from contextlib import chdir
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
# The formatter is a checkout tool rather than an installed package.
sys.path.insert(0, str(ROOT / "tools"))
import format_cpp  # noqa: E402


class CppStyleTest(unittest.TestCase):
    """Protect generated sources and prevent formatting with the wrong version."""

    def setUp(self):
        """Create an isolated fixture repository under the configured TMPDIR."""
        self.temp = tempfile.TemporaryDirectory(
            prefix="t150-cpp-style-test-", dir=os.environ["TMPDIR"]
        )
        self.addCleanup(self.temp.cleanup)
        self.temp_root = Path(self.temp.name).resolve()
        self.root = self.temp_root / "repo"
        self.root.mkdir()
        self.git(self.root, "init", "--quiet")
        (self.root / ".clang-format").write_text("BasedOnStyle: Google\n")

    def git(self, root, *arguments):
        """Run fixture Git commands without inherited configuration or hooks."""
        env = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith("GIT_")
        }
        env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        subprocess.run(
            ["git", "-c", f"core.hooksPath={os.devnull}", "-C", str(root), *arguments],
            env=env,
            check=True,
            capture_output=True,
        )

    def track(self, root, *names):
        """Create and stage fixture files without committing them."""
        for name in names:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("int answer(){return 42;}\n")
        self.git(root, "add", "--", *names)

    def fake_formatter(self, banner):
        """Create a formatter stub that saves non-version invocation arguments."""
        binary = self.root / "fake-clang-format"
        record = self.root / "formatter-argv.json"
        binary.write_text(
            f"#!{sys.executable}\n"
            "import json\n"
            "from pathlib import Path\n"
            "import sys\n"
            "if sys.argv[1:] == ['--version']:\n"
            f"    print({banner!r})\n"
            "else:\n"
            f"    Path({str(record)!r}).write_text(json.dumps(sys.argv[1:]))\n"
        )
        binary.chmod(0o755)
        return binary, record

    def test_selection_includes_new_tracked_sources_and_excludes_protected_paths(self):
        """Select staged source extensions while excluding protected and local files."""
        selected = {
            "components/one.c",
            "components/two.cc",
            "components/three.cpp",
            "tests/four.cxx",
            "tests/five.h",
            "tests/six.hpp",
            "tools/test_reset_speed_header.cpp",
            "tools/test_tb01_core.c",
            "tools/patches/reset_speed.c",
            "tools/patches/uart_parser.c",
            "tools/patches/tb01.c",
            "tools/patches/tb01_core.c",
            "tools/patches/tb01_core.h",
            "tools/patches/platform_runtime.c",
            "tools/patches/platform_runtime.h",
            "tools/patches/platform_tb01.c",
        }
        protected = {
            "components/desk_firmware_update/ui.h",
            "components/desk_firmware_update/child_lock_patch.h",
            "components/desk_firmware_update/reset_speed_patch.h",
            "components/desk_firmware_update/dt02_patch.h",
            "components/desk_firmware_update/tb01_patch.h",
            "components/desk_firmware_update/platform_patch.h",
            "components/desk_uart/firmware_layout.h",
            "lib/desk_protocol/include/desk_protocol/protocol.h",
            "tools/patches/reset_speed.S",
            "tools/patches/reset_speed_core.S",
            "tools/patches/tb01_core.S",
            "tools/patches/tb01_platform.S",
            "tools/patches/tb01_hooks.S",
            "tools/patches/platform_boot.S",
            "tools/patches/platform_layout.h",
            "tools/patches/unselected.c",
            "tools/other.cpp",
            "build/generated.cpp",
            "third_party/vendor.h",
        }
        self.track(self.root, *sorted(selected | protected))
        (self.root / "components/local.hpp").write_text("int local;\n")
        self.assertEqual(
            set(format_cpp.source_files(self.root)), {self.root / p for p in selected}
        )
        self.track(self.root, "tests/new_source.hpp")
        self.assertEqual(
            set(format_cpp.source_files(self.root)),
            {self.root / p for p in selected | {"tests/new_source.hpp"}},
        )

    def test_inherited_git_environment_cannot_select_another_repository(self):
        """Ignore Git directory, worktree, index and injected-config redirects."""
        self.track(self.root, "components/expected.cpp")
        foreign = self.temp_root / "foreign"
        foreign.mkdir()
        self.git(foreign, "init", "--quiet")
        self.track(foreign, "components/foreign.hpp")
        with patch.dict(
            os.environ,
            {
                "GIT_DIR": str(foreign / ".git"),
                "GIT_WORK_TREE": str(foreign),
                "GIT_INDEX_FILE": str(foreign / ".git/index"),
                "GIT_CONFIG_COUNT": "1",
                "GIT_CONFIG_KEY_0": "core.worktree",
                "GIT_CONFIG_VALUE_0": str(foreign),
            },
        ):
            self.assertEqual(
                format_cpp.source_files(self.root),
                [self.root / "components/expected.cpp"],
            )

    def test_source_and_parent_symlinks_are_rejected(self):
        """Reject selected symlinks even when their targets stay inside the fixture."""
        for kind in ("file", "parent"):
            with self.subTest(kind=kind):
                root = self.temp_root / kind
                root.mkdir()
                self.git(root, "init", "--quiet")
                self.track(root, "components/source.hpp")
                if kind == "file":
                    target = root / "target.hpp"
                    target.write_text("int protected_value;\n")
                    source = root / "components/source.hpp"
                    source.unlink()
                    source.symlink_to(target)
                else:
                    target = root / "saved-components"
                    (root / "components").rename(target)
                    (root / "components").symlink_to(target, target_is_directory=True)
                with self.assertRaisesRegex(ValueError, "symlink"):
                    format_cpp.source_files(root)

    def test_wrong_version_is_rejected_before_fix_can_invoke_formatter(self):
        """Reject nearby patch versions and suffixes before any formatting call."""
        self.track(self.root, "components/source.cpp")
        source = self.root / "components/source.cpp"
        original = source.read_bytes()
        for version in ("23.1.01", "23.1.4", "23.1.0git"):
            with self.subTest(version=version):
                binary, marker = self.fake_formatter(f"clang-format version {version}")
                with self.assertRaisesRegex(ValueError, "23.1.0 required"):
                    format_cpp.format_sources(self.root, binary, fix=True)
                self.assertFalse(marker.exists())
                self.assertEqual(source.read_bytes(), original)

    def test_modes_use_root_style_and_selected_paths_with_a_relative_tool(self):
        """Pass root style and exact files in both modes using the verified tool."""
        selected = [
            "components/nested/source.cpp",
            "tests/helper.h",
            "tools/test_reset_speed_header.cpp",
        ]
        self.track(self.root, *selected)
        self.track(self.root, "components/desk_firmware_update/ui.h")
        (self.root / "components/nested/local.cpp").write_text("int local;\n")
        (self.root / "components/nested/.clang-format").write_text(
            "BasedOnStyle: LLVM\nIndentWidth: 8\n"
        )
        binary, record = self.fake_formatter("clang-format version 23.1.0")
        for fix in (False, True):
            with self.subTest(fix=fix):
                with chdir(self.temp_root):
                    count = format_cpp.format_sources(
                        self.root, binary.relative_to(self.temp_root), fix=fix
                    )
                self.assertEqual(count, len(selected))
                flags = ["-i"] if fix else ["--dry-run", "--Werror"]
                self.assertEqual(
                    json.loads(record.read_text()),
                    [
                        f"--style=file:{self.root / '.clang-format'}",
                        *flags,
                        "--",
                        *(str(self.root / name) for name in selected),
                    ],
                )


if __name__ == "__main__":
    unittest.main()
