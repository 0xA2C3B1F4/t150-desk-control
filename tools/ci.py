#!/usr/bin/env python3
"""Select PR checks conservatively and verify their combined result."""

import argparse
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess

FULL_JOBS = {
    "style",
    "native",
    "esphome",
    "github-example",
    "release-checks",
    "release-examples",
}


def documentation_path(path):
    """Allow only prose documents to skip builds, excluding release notes."""
    parts = PurePosixPath(path).parts
    return path == "README.md" or (
        len(parts) >= 2 and parts[0] == "docs" and path.endswith(".md")
    )


def select_checks(root, event_name, event, force_full=False):
    """Use the entire PR diff; missing metadata or Git errors require all checks."""
    if force_full or event_name != "pull_request":
        return True, "Full checks for main, manual and reusable workflow runs"
    try:
        pr = event["pull_request"]
        base, head = pr["base"]["sha"], pr["head"]["sha"]
        if not all(
            isinstance(ref, str) and re.fullmatch(r"[0-9a-f]{40}", ref)
            for ref in (base, head)
        ):
            raise ValueError("Invalid PR revisions")
        fields = (
            subprocess.check_output(
                [
                    "git",
                    "-C",
                    str(root),
                    "diff",
                    "--name-status",
                    "--no-renames",
                    "-z",
                    f"{base}...{head}",
                    "--",
                ],
                env={k: v for k, v in os.environ.items() if not k.startswith("GIT_")},
                stderr=subprocess.PIPE,
                timeout=30,
            )
            .decode()
            .split("\0")
        )
        if fields[-1] != "" or len(fields[:-1]) % 2:
            raise ValueError("Malformed Git diff")
        changes = list(zip(fields[:-1:2], fields[1::2], strict=True))
    except (KeyError, TypeError, ValueError, OSError, subprocess.SubprocessError):
        return True, "PR diff unavailable; running all checks"
    if changes and all(
        status in {"A", "M"} and documentation_path(path) for status, path in changes
    ):
        return False, "Only README.md or Markdown under docs/ added or modified"
    return True, "PR includes code, configuration, release inputs or other files"


def check_results(needs):
    """Reject failed prerequisites, skipped full checks and unexpected job sets."""
    if set(needs) != FULL_JOBS | {"changes"}:
        raise ValueError("Unexpected CI job set")
    if needs["changes"].get("result") != "success":
        raise ValueError("Change selection or documentation checks failed")
    full = needs["changes"].get("outputs", {}).get("full")
    if full not in {"true", "false"}:
        raise ValueError("Missing check selection")
    expected = "success" if full == "true" else "skipped"
    if any(needs[job].get("result") != expected for job in FULL_JOBS):
        raise ValueError("Selected checks failed, were cancelled or were skipped")


def main():
    """Write the selected scope or validate job results supplied by GitHub."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("select", "result"))
    args = parser.parse_args()
    if args.command == "result":
        check_results(json.loads(os.environ["CI_NEEDS"]))
        print("Selected CI checks passed")
        return
    try:
        event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text())
    except (KeyError, OSError, ValueError):
        event = {}
    full, reason = select_checks(
        Path.cwd(),
        os.environ.get("GITHUB_EVENT_NAME", ""),
        event,
        force_full=os.environ.get("CI_FULL_SUITE") == "true"
        or bool(os.environ.get("CI_SOURCE_REF")),
    )
    with Path(os.environ["GITHUB_OUTPUT"]).open("a") as output:
        output.write(f"full={str(full).lower()}\n")
    print(reason)


if __name__ == "__main__":
    main()
