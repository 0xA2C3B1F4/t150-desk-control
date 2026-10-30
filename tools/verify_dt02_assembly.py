#!/usr/bin/env python3
"""Rebuild our DT02 replacement code and compare with the embedded byte array.

No vendor image or hardware access. Run from the checkout root with LLVM tools
in PATH, or supply --llvm-bin. Temporary build files use TMPDIR and are removed.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--llvm-bin", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
if not os.environ.get("TMPDIR"):
    parser.error("TMPDIR must name a writable temporary directory")
def tool(name):
    candidate = args.llvm_bin / name if args.llvm_bin else Path(shutil.which(name) or name)
    if not candidate.exists():
        found = shutil.which(name)
        if not found: parser.error(f"Required LLVM tool not found: {name}")
        candidate = Path(found)
    return str(candidate)
header = (root / "components/desk_firmware_update/dt02_patch.h").read_text()
array = re.search(r"DT02_CODE\[\] = \{(.*?)\};", header, re.S).group(1)
expected = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", array))
with tempfile.TemporaryDirectory(prefix="t150-dt02-", dir=os.environ["TMPDIR"]) as temporary:
    work = Path(temporary)
    layout = work / "layout.ld"
    layout.write_text('ENTRY(direct_target_hook)\nSECTIONS { .hook 0x08007bae : { *(.hook) } .cave 0x0800880c : { *(.cave) } /DISCARD/ : { *(.ARM.attributes) *(.comment) } }\nASSERT(SIZEOF(.hook) == 4, "hook size")\nASSERT(SIZEOF(.cave) <= 1012, "cave overflow")\n')
    subprocess.run([tool("clang"), "--target=arm-none-eabi", "-mcpu=cortex-m0", "-mthumb", "-c", str(root / "tools/patches/dt02.S"), "-o", str(work / "patch.o")], check=True)
    subprocess.run([tool("ld.lld"), "-T", str(layout), str(work / "patch.o"), "-o", str(work / "patch.elf")], check=True)
    for section in ("hook", "cave"):
        subprocess.run([tool("llvm-objcopy"), "-O", "binary", "--only-section=." + section, str(work / "patch.elf"), str(work / (section + ".bin"))], check=True)
    if (work / "hook.bin").read_bytes() != bytes.fromhex("00f02dfe") or (work / "cave.bin").read_bytes() != expected:
        raise SystemExit("Assembly does not match the embedded DT02 hook and code")
print(f"DT02 hook and {len(expected)} replacement-code bytes reproduced")
