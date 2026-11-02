#!/usr/bin/env python3
"""Install hash-pinned Node for formatting without replacing the runner's Node."""

import hashlib
import os
from pathlib import Path
import platform
import subprocess
import tarfile
import urllib.request

VERSION = "26.13.0"
ARCHIVE = f"node-v{VERSION}-linux-x64.tar.xz"
URL = f"https://nodejs.org/dist/v{VERSION}/{ARCHIVE}"
SHA256 = "d28a96af6c38596f6d9d97c0ee531c13d238d757742387d1928de822d959e997"


def main():
    """Expose the verified Node path to the formatter through T150_STYLE_NODE."""
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        raise SystemExit("This installer requires the Linux x86_64 CI runner")
    repo = Path(__file__).resolve().parents[1]
    if (repo / ".node-version").read_text().strip() != VERSION:
        raise SystemExit("Node installer and .node-version differ")
    root = Path(os.environ["TMPDIR"]) / "desk-style-node"
    root.mkdir(exist_ok=False)
    archive = root / ARCHIVE
    with urllib.request.urlopen(URL, timeout=60) as response, archive.open("wb") as out:
        digest = hashlib.sha256()
        while data := response.read(1024 * 1024):
            digest.update(data)
            out.write(data)
    if digest.hexdigest() != SHA256:
        raise SystemExit("Node archive checksum mismatch")
    with tarfile.open(archive) as tar:
        tar.extractall(root, filter="data")
    archive.unlink()
    node = root / f"node-v{VERSION}-linux-x64/bin/node"
    version = subprocess.check_output(
        [str(node), "--version"], text=True, timeout=10
    ).strip()
    if version != f"v{VERSION}":
        raise SystemExit("Unexpected Node version")
    with Path(os.environ["GITHUB_ENV"]).open("a") as out:
        out.write(f"T150_STYLE_NODE={node}\n")
    print(f"Formatter Node: {version}")


if __name__ == "__main__":
    main()
