"""Advisory source identity; copied components also work without Git."""

import json
import os
from pathlib import Path
import re
import subprocess

VERSION_PATTERN = r"[0-9]{4}\.(?:[1-9]|1[0-2])\.(?:0|[1-9][0-9]*)"


def read_version(component=None):
    component = Path(component or Path(__file__).parent)
    data = json.loads((component / "project_version.json").read_text())
    if (
        type(data.get("schema_version")) is not int
        or data["schema_version"] != 1
        or not isinstance(data.get("version"), str)
        or not re.fullmatch(VERSION_PATTERN, data["version"])
    ):
        raise ValueError("Invalid project_version.json")
    return data["version"]


def source_metadata(component=None):
    component = Path(component or Path(__file__).parent).resolve()
    result = {"version": "unknown", "revision": "unknown", "state": "unknown"}
    try:
        result["version"] = read_version(component)
    except (OSError, ValueError, TypeError, AttributeError):
        pass
    # A parent config repository or caller's Git environment is not our identity.
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env.update(GIT_TERMINAL_PROMPT="0", GIT_OPTIONAL_LOCKS="0")

    def git(*args):
        return subprocess.run(
            ["git", "-C", str(component), *args],
            env=env,
            check=True,
            capture_output=True,
            text=True,
            timeout=3,
        ).stdout.strip()

    try:
        root = Path(git("rev-parse", "--show-toplevel")).resolve()
        if (root / "components/desk_uart").resolve() != component:
            return result
        manifest = json.loads((root / "library.json").read_text())
        if manifest.get("name") != "desk_protocol":
            return result
        revision = git("rev-parse", "HEAD")
        if not re.fullmatch(r"[0-9a-f]{40}", revision):
            return result
        state = (
            "dirty"
            if git("status", "--porcelain", "--untracked-files=all")
            else "clean"
        )
        result.update(revision=revision, state=state)
    except (OSError, ValueError, TypeError, AttributeError, subprocess.SubprocessError):
        pass
    return result
