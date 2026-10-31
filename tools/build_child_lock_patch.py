#!/usr/bin/env python3
"""Build the hash-bound CL01 offline candidate from a saved DT02 image.

No device/serial/network interfaces. Vendor image bytes are never embedded here.
LLVM compilation uses TMPDIR and removes temporary files on success or failure.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE_FULL_SHA256 = "6e47223e955c0b4079c9e76b305e254346e70a159a470cd6fb5a06da6ea02c3a"
SOURCE_APP_SHA256 = "e5324473e0c7cdb43b0a3328d023454f56724875ab61973a5640c683727dabff"
OUTPUT_FULL_SHA256 = "a63a99c656e34f5ffbdab635bdb320832796359f4b26c7f0d8d4eb2acde7048b"
OUTPUT_APP_SHA256 = "81933d93b8bc28ef0eb8b29bf7ae1c7b9104f0a8e03c1c2f8096d509de0f75d3"
FLASH_BASE, APP_OFFSET, FULL_SIZE, APP_SIZE = 0x08000000, 0x2800, 65536, 55296
SECTIONS = {"gpio_hook": (0x4388, 8), "boot_hook": (0x85F8, 4),
            "hook": (0x7BAE, 4), "cave": (0x880C, 1012),
            "child_lock": (0x8C00, 2048)}


class ImageError(ValueError):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def identify(data: bytes) -> int:
    if len(data) == FULL_SIZE and sha256(data) == SOURCE_FULL_SHA256:
        return 0
    if len(data) == APP_SIZE and sha256(data) == SOURCE_APP_SHA256:
        return APP_OFFSET
    raise ImageError("Input must be the exact saved DT02 full or application image")


def read_saved_image(path: Path) -> bytes:
    """Read only a bounded, saved regular file. Never open a device or FIFO."""
    if not hasattr(os, "O_NOFOLLOW") or not hasattr(os, "O_NONBLOCK"):
        raise ImageError("Safe saved-image reads require O_NOFOLLOW and O_NONBLOCK")
    before = path.lstat()
    if not stat.S_ISREG(before.st_mode) or before.st_size not in (FULL_SIZE, APP_SIZE):
        raise ImageError("Source must be a regular saved full/application image file")
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, "rb") as file:
        info = os.fstat(file.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size not in (FULL_SIZE, APP_SIZE):
            raise ImageError("Source must be a regular saved full/application image file")
        return file.read(FULL_SIZE + 1)


def find_tool(name: str, llvm_bin: Path | None = None) -> str:
    candidate = llvm_bin / name if llvm_bin else None
    if candidate and candidate.is_file():
        return str(candidate)
    found = shutil.which(name)
    if not found:
        raise ImageError(f"Required LLVM tool not found: {name}")
    return found


def compile_patch(llvm_bin: Path | None = None) -> tuple[dict[str, bytes], dict[str, int]]:
    task_tmp = os.environ.get("TMPDIR")
    if not task_tmp or not Path(task_tmp).is_dir() or not os.access(task_tmp, os.W_OK):
        raise ImageError("TMPDIR must name an existing writable temporary directory")
    clang = find_tool("clang", llvm_bin)
    linker = find_tool("ld.lld", llvm_bin)
    objcopy = find_tool("llvm-objcopy", llvm_bin)
    nm = find_tool("llvm-nm", llvm_bin)
    with tempfile.TemporaryDirectory(prefix="t150-child-lock-build-", dir=task_tmp) as directory:
        work = Path(directory)
        layout = ["ENTRY(child_lock_dispatch)", "SECTIONS {"]
        for section, (offset, capacity) in SECTIONS.items():
            layout.append(f" .{section} {FLASH_BASE + offset:#x} : {{ *(.{section}) }}")
        layout += [" /DISCARD/ : { *(.ARM.attributes) *(.comment) }", "}"]
        for section, (_, capacity) in SECTIONS.items():
            layout.append(f'ASSERT(SIZEOF(.{section}) <= {capacity}, "{section} overflow")')
        for section, length in (("gpio_hook", 8), ("boot_hook", 4), ("hook", 4)):
            layout.append(f'ASSERT(SIZEOF(.{section}) == {length}, "{section} length")')
        (work / "layout.ld").write_text("\n".join(layout) + "\n")
        objects = []
        for source in ("dt02.S", "child_lock.S"):
            output = work / (source + ".o")
            subprocess.run([clang, "--target=arm-none-eabi", "-mcpu=cortex-m0", "-mthumb",
                            "-DCHILD_LOCK=1", "-c", str(ROOT / "tools/patches" / source),
                            "-o", str(output)], check=True, capture_output=True)
            objects.append(str(output))
        elf = work / "patch.elf"
        subprocess.run([linker, "-T", str(work / "layout.ld"), *objects, "-o", str(elf)],
                       check=True, capture_output=True)
        sections = {}
        for section in SECTIONS:
            binary = work / (section + ".bin")
            subprocess.run([objcopy, "-O", "binary", "--only-section=." + section,
                            str(elf), str(binary)], check=True, capture_output=True)
            sections[section] = binary.read_bytes()
        symbol_text = subprocess.run([nm, "--defined-only", "--numeric-sort", str(elf)],
                                     check=True, capture_output=True, text=True).stdout
        symbols = {}
        for line in symbol_text.splitlines():
            fields = line.split()
            if len(fields) == 3 and fields[2].startswith("child_lock_"):
                symbols[fields[2]] = int(fields[0], 16) & ~1
        return sections, symbols


def build(data: bytes, llvm_bin: Path | None = None) -> tuple[bytes, dict]:
    image_origin = identify(data)
    sections, symbols = compile_patch(llvm_bin)
    # A second, localized check makes the code/data reservation reviewable.
    checks = {0x4388: bytes.fromhex("0246128a00200a42"),
              0x85F8: bytes.fromhex("fff712f8"),
              0x7BAE: bytes.fromhex("00f02dfe")}
    for offset, expected in checks.items():
        if data[offset-image_origin:offset-image_origin+len(expected)] != expected:
            raise ImageError(f"Unexpected original hook at {FLASH_BASE+offset:#x}")
    if data[0x8C00-image_origin:0x9400-image_origin] != b"\xff" * 2048:
        raise ImageError("CL01 code reservation is not erased in source")
    output = bytearray(data)
    spans = []
    for section, replacement in sections.items():
        offset, _ = SECTIONS[section]
        output[offset-image_origin:offset-image_origin+len(replacement)] = replacement
        spans.append({"section": section, "address": FLASH_BASE + offset,
                      "image_offset": offset - image_origin, "size": len(replacement),
                      "replacement_sha256": sha256(replacement)})
    result = bytes(output)
    # Every changed byte must lie in a declared span. Bootloader and tail stay exact.
    allowed = {item["image_offset"] + i for item in spans for i in range(item["size"])}
    if len(result) != len(data) or any(a != b and i not in allowed
                                       for i, (a, b) in enumerate(zip(data, result))):
        raise ImageError("Builder changed bytes outside declared spans")
    expected_output = OUTPUT_FULL_SHA256 if image_origin == 0 else OUTPUT_APP_SHA256
    if sha256(result) != expected_output:
        raise ImageError("Compiled output does not match the reviewed CL01 image hash")
    manifest = {"schema_version": 1, "patch_id": "t150-dt02-child-lock-cl01", "candidate_revision": 3,
                "offline_only": True, "hardware_tested": False, "installable_approved": False,
                "source_sha256": sha256(data), "source_size": len(data),
                "output_sha256": sha256(result), "output_size": len(result),
                "output_application_sha256": sha256(result[APP_OFFSET-image_origin:]),
                "image_origin": image_origin, "patch_spans": spans, "symbols": symbols,
                "ram": {"locked": 0x20000FC8, "physical_gate": 0x20000FC9,
                        "storage_fault": 0x20000FCA, "reserved_end_exclusive": 0x20000FD0},
                "eeprom": {"addresses": [0x60, 0x61], "locked": [0xA5, 0x5A],
                           "unlocked": [0x5A, 0xA5], "invalid_is_locked": True},
                "source_files": {str(path.relative_to(ROOT)): sha256(path.read_bytes())
                                 for path in (ROOT / "tools/patches/dt02.S",
                                              ROOT / "tools/patches/child_lock.S",
                                              Path(__file__).resolve())}}
    return result, manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--llvm-bin", type=Path)
    args = parser.parse_args()
    for path in (args.output, args.manifest):
        if path.exists():
            parser.error(f"Refusing to overwrite existing path: {path}")
        if not path.parent.is_dir():
            parser.error(f"Output parent does not exist: {path.parent}")
    try:
        result, manifest = build(read_saved_image(args.source), args.llvm_bin)
    except (ImageError, OSError, subprocess.CalledProcessError) as error:
        if isinstance(error, subprocess.CalledProcessError):
            parser.error(error.stderr.decode() if isinstance(error.stderr, bytes) else str(error.stderr))
        parser.error(str(error))
    manifest["reproduce"] = ["python3", "tools/build_child_lock_patch.py", "<saved-DT02-image>",
                              "<new-output>", "--manifest", "<new-manifest>",
                              "--llvm-bin", "<LLVM-bin-directory>"]
    with args.output.open("xb") as file:
        file.write(result)
    try:
        with args.manifest.open("x") as file:
            file.write(json.dumps(manifest, indent=2) + "\n")
    except BaseException:
        args.output.unlink()
        raise
    print(json.dumps({"patch_id": manifest["patch_id"], "output_sha256": manifest["output_sha256"],
                      "output_size": len(result), "hardware_tested": False}, indent=2))


if __name__ == "__main__":
    main()
