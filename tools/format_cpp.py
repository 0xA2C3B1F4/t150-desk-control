#!/usr/bin/env python3
"""Check or format the repository's tracked C/C++ sources with pinned clang-format.

Generated firmware headers, include wrappers and patch sources stay outside the
selection. Both modes use the repository's root .clang-format configuration.
"""

import argparse
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess

from install_ci_llvm import verify_clang_format

ROOT = Path(__file__).resolve().parents[1]
SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"}
EXTRA_SOURCE = "tools/test_reset_speed_header.cpp"
EXCLUDED = {
    "components/desk_firmware_update/ui.h",
    "components/desk_firmware_update/child_lock_patch.h",
    "components/desk_firmware_update/reset_speed_patch.h",
    "components/desk_firmware_update/dt02_patch.h",
}


def _git_output(root, *arguments):
    """Read Git metadata without inherited repository or configuration overrides."""
    env = {
        key: value for key, value in os.environ.items() if not key.startswith("GIT_")
    }
    env.update(
        GIT_CONFIG_NOSYSTEM="1",
        GIT_CONFIG_GLOBAL=os.devnull,
        GIT_OPTIONAL_LOCKS="0",
        GIT_TERMINAL_PROMPT="0",
    )
    return subprocess.check_output(
        ["git", "-C", str(root), *arguments], env=env, timeout=30
    )


def source_files(root):
    """Return tracked sources in scope, rejecting missing files and symlinks.

    Newly staged files are included. Untracked files and sources outside the
    declared components/tests paths and explicit tool fixture are excluded.
    """
    root = Path(root).resolve()
    top = Path(os.fsdecode(_git_output(root, "rev-parse", "--show-toplevel")).strip())
    if top.resolve() != root:
        raise ValueError("C/C++ formatting requires the repository root")
    selected = set()
    for name in _git_output(root, "ls-files", "--cached", "-z").split(b"\0"):
        if not name:
            continue
        relative = PurePosixPath(os.fsdecode(name))
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("Git returned a source path outside the repository")
        if str(relative) in EXCLUDED:
            continue
        in_tree = relative.parts[0] in {"components", "tests"}
        if not (
            in_tree and relative.suffix in SUFFIXES or str(relative) == EXTRA_SOURCE
        ):
            continue
        path = root
        for part in relative.parts:
            path /= part
            if path.is_symlink():
                raise ValueError(f"Refusing to format a symlink: {relative}")
        if not path.is_file():
            raise ValueError(f"Tracked source is missing or not a file: {relative}")
        selected.add(path)
    return sorted(selected)


def format_sources(root, clang_format, *, fix):
    """Check or rewrite the selected sources after version and path validation.

    Check mode uses --dry-run --Werror. Both modes explicitly use the root style
    file, including for sources below directories with their own configuration.
    Return the number of selected files after a successful formatter invocation.
    """
    root = Path(root).resolve()
    sources = source_files(root)
    if not sources:
        raise ValueError("No tracked C/C++ sources are selected")
    style = root / ".clang-format"
    if style.is_symlink() or not style.is_file():
        raise ValueError("The repository root needs a regular .clang-format file")
    resolved = shutil.which(str(clang_format))
    if resolved is None:
        raise ValueError(f"Formatter executable not found: {clang_format}")
    binary = Path(resolved).resolve()
    verify_clang_format(binary)
    mode = ["-i"] if fix else ["--dry-run", "--Werror"]
    subprocess.run(
        [str(binary), f"--style=file:{style}", *mode, "--", *map(str, sources)],
        cwd=root,
        check=True,
    )
    return len(sources)


def main(argv=None):
    """Parse the mode and formatter command, then check or update selected sources."""
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument(
        "--check", action="store_true", help="Check without changing files"
    )
    mode.add_argument(
        "--fix", action="store_true", help="Format selected tracked files"
    )
    parser.add_argument(
        "--clang-format",
        default="clang-format",
        help="Formatter executable path or command",
    )
    args = parser.parse_args(argv)
    try:
        count = format_sources(ROOT, args.clang_format, fix=args.fix)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
    print(f"{'Formatted' if args.fix else 'Checked'} {count} C/C++ source files.")


if __name__ == "__main__":
    main()
