#!/usr/bin/env python3
"""Format the tracked asset sources with pinned Node and Prettier."""

import argparse
from fnmatch import fnmatchcase
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GROUPS = {
    "web": (
        "components/desk_firmware_update/ui.html",
        "tests/esphome/firmware_ui_test.cjs",
    ),
    "data": ("*.yaml", "*.yml", "*.json"),
    "markdown": ("*.md",),
}

EXCLUDED_PARTS = {
    "build",
    "node_modules",
    ".esphome",
    ".pio",
    ".pioenvs",
    ".piolibdeps",
    "third_party",
    "vendor",
}
EXCLUDED_FILES = {"package-lock.json", "tools/style/package-lock.json"}


def selected_files(root, groups):
    """Select tracked sources while excluding generated and third-party data."""
    tracked = set(
        subprocess.check_output(
            ["git", "-C", str(root), "ls-files", "-z"], text=True
        ).split("\0")
    )
    files = sorted(
        name
        for name in tracked
        if name
        and name not in EXCLUDED_FILES
        and not EXCLUDED_PARTS.intersection(Path(name).parts)
        and any(
            fnmatchcase(name, pattern) for group in groups for pattern in GROUPS[group]
        )
    )
    for name in files:
        path = root / name
        if (
            path.is_symlink()
            or not path.is_file()
            or not path.resolve().is_relative_to(root.resolve())
        ):
            raise SystemExit(
                f"Expected a regular source file inside the repository: {name}"
            )
    return files


def require_version(command, expected):
    """Reject a tool whose complete version string differs from the pinned version."""
    actual = subprocess.check_output(
        [*command, "--version"], text=True, timeout=15
    ).strip()
    if actual != expected:
        raise SystemExit(f"Expected {expected}; got {actual!r} from {command[0]}")


def run_prettier(node, prettier, files, mode):
    """Check the formatter identity, then run it with the repository's configuration."""
    version = json.loads((ROOT / "tools/style/package.json").read_text())[
        "devDependencies"
    ]["prettier"]
    require_version([str(node), str(prettier)], version)
    subprocess.run(
        [
            str(node),
            str(prettier),
            mode,
            "--config",
            str(ROOT / ".prettierrc.json"),
            "--ignore-path",
            str(ROOT / ".prettierignore"),
            *files,
        ],
        cwd=ROOT,
        check=True,
    )


def main():
    """Install locked dependencies in TMPDIR and format only the selected sources."""
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    for mode in ("check", "write", "debug-check"):
        modes.add_argument(
            f"--{mode}", dest="mode", action="store_const", const=f"--{mode}"
        )
    parser.add_argument("--group", action="append", choices=sorted(GROUPS))
    parser.add_argument("--node", default=os.environ.get("T150_STYLE_NODE", "node"))
    parser.add_argument(
        "--prettier", type=Path, help="Existing pinned bin/prettier.cjs"
    )
    args = parser.parse_args()
    node = Path(shutil.which(args.node) or args.node).resolve()
    version = (ROOT / ".node-version").read_text().strip()
    require_version([str(node)], f"v{version}")
    files = selected_files(ROOT, args.group or sorted(GROUPS))
    if not files:
        raise SystemExit("No tracked sources selected")
    if args.prettier:
        run_prettier(node, args.prettier.resolve(), files, args.mode)
        return
    tmp = Path(os.environ["TMPDIR"]).resolve()
    npm = node.with_name("npm").resolve()
    if not npm.is_file():
        raise SystemExit("The selected Node installation must include npm")
    env = dict(os.environ, npm_config_cache=str(tmp / "desk-style-npm-cache"))
    with tempfile.TemporaryDirectory(prefix="desk-prettier-", dir=tmp) as directory:
        prefix = Path(directory)
        for name in ("package.json", "package-lock.json"):
            shutil.copyfile(ROOT / "tools/style" / name, prefix / name)
        subprocess.run(
            [
                str(node),
                str(npm),
                "ci",
                "--ignore-scripts",
                "--no-audit",
                "--no-fund",
            ],
            cwd=prefix,
            env=env,
            check=True,
        )
        run_prettier(
            node, prefix / "node_modules/prettier/bin/prettier.cjs", files, args.mode
        )


if __name__ == "__main__":
    main()
