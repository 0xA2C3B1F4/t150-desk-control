#!/usr/bin/env python3
"""Install the hash-pinned Linux LLVM used to reproduce embedded patch bytes."""
import hashlib
import os
from pathlib import Path
import platform
import subprocess
import tarfile
import urllib.request

VERSION = "23.1.0"
ARCHIVE = "LLVM-23.1.0-Linux-X64.tar.xz"
URL = "https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.0/" + ARCHIVE
SHA256 = "18da30f77f475688a18f7704d23f9f155ae007ed9922dbed6850a9419d9fec8c"


def main():
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        raise SystemExit("This installer is for the standard x86_64 Linux CI runner")
    root = Path(os.environ["TMPDIR"]) / "desk-llvm"
    root.mkdir(exist_ok=False)
    archive = root / ARCHIVE
    with urllib.request.urlopen(URL,timeout=120) as response, archive.open("wb") as out:
        digest = hashlib.sha256()
        while data := response.read(1024*1024):
            digest.update(data);out.write(data)
    if digest.hexdigest() != SHA256:
        raise SystemExit("LLVM archive checksum mismatch")
    with tarfile.open(archive) as tar:
        members = []
        for item in tar:
            parts = Path(item.name).parts
            if len(parts)<2:
                continue
            relative = Path(*parts[1:])
            # Skip static libraries, tests and documentation to limit runner disk use.
            if parts[1]=="bin" or str(relative).startswith("lib/clang/") or (
                    parts[1]=="lib" and ".so" in relative.name):
                members.append(item)
        tar.extractall(root,members=members,filter="data")
    archive.unlink()
    bins = list(root.glob("*/bin/clang"))
    if len(bins)!=1:
        raise SystemExit("Unexpected LLVM archive layout")
    binary = bins[0]
    info = subprocess.check_output([str(binary),"--version"],text=True)
    if "clang version " + VERSION not in info:
        raise SystemExit("Unexpected LLVM compiler version")
    print(info,end="")
    with open(os.environ["GITHUB_PATH"],"a") as out:
        out.write(str(binary.parent)+"\n")


if __name__=="__main__":main()
