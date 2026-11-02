#!/usr/bin/env python3
"""Build RS01 offline from the exact saved DT02+CL01 R3 full/app image."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from build_child_lock_patch import (
    APP_OFFSET,
    APP_SIZE,
    FLASH_BASE,
    FULL_SIZE,
    OUTPUT_APP_SHA256 as SOURCE_APP_SHA256,
    OUTPUT_FULL_SHA256 as SOURCE_FULL_SHA256,
    ROOT,
    ImageError,
    compile_patch as compile_cl01,
    find_tool,
    read_saved_image,
    sha256,
)
from generate_reset_speed_core import check_source

PAYLOAD_SIZE = 32768
SECTIONS = {
    "rs_vector": (0x283C, 4),
    "rs_boot_hook": (0x85F8, 4),
    "rs_uart_hook": (0x7BAE, 4),
    "rs_main_hook": (0x8688, 4),
    "rs_parser_hook": (0x44F4, 8),
    "rs_rx_reset_hook": (0x7636, 4),
    "rs_speed_hook": (0x63C8, 4),
    "rs_output_hook": (0x70D8, 8),
    "rs_complete_hook": (0x5C0C, 4),
    "reset_speed": (0x9400, 4096),
}
# These pins are filled only after reviewable deterministic compilation.
OUTPUT_FULL_SHA256 = "96fe9d27e3e0f5ab2654c2552ae4e914595917b3e7b1dbbb127e773183c5a534"
OUTPUT_APP_SHA256 = "ad1f321c570de25abef0f106175a5722d709b92e5db6d3da792bed9cc55fe78b"


def compile_patch(llvm_bin=None):
    """Compile RS01 sections in TMPDIR and return their bytes and symbols.

    Check the generated C-source marker and link against CL01 symbols. External
    LLVM tools perform the build, and temporary files are removed on exit.
    """
    check_source()
    task_tmp = os.environ.get("TMPDIR")
    if not task_tmp or not Path(task_tmp).is_dir() or not os.access(task_tmp, os.W_OK):
        raise ImageError("TMPDIR must name a writable existing directory")
    clang = find_tool("clang", llvm_bin)
    linker = find_tool("ld.lld", llvm_bin)
    objcopy = find_tool("llvm-objcopy", llvm_bin)
    nm = find_tool("llvm-nm", llvm_bin)
    _, cl_symbols = compile_cl01(llvm_bin)
    with tempfile.TemporaryDirectory(
        prefix="t150-rs01-build-", dir=task_tmp
    ) as directory:
        work = Path(directory)
        layout = ["ENTRY(rs_dispatch)", "SECTIONS {"]
        for section, (offset, capacity) in SECTIONS.items():
            extra = " *(.text*) *(.rodata*)" if section == "reset_speed" else ""
            layout.append(
                f" .{section} {FLASH_BASE + offset:#x} : {{ *(.{section}){extra} }}"
            )
        layout += [
            " /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.ARM.exidx*) *(.ARM.extab*) }",
            "}",
        ]
        for name, value in cl_symbols.items():
            layout.append(f"{name} = {value | 1:#x};")
        layout.append("rs_stock_rx_init = 0x08007f8d;")
        for section, (_, capacity) in SECTIONS.items():
            relation = "<=" if section == "reset_speed" else "=="
            layout.append(
                f'ASSERT(SIZEOF(.{section}) {relation} {capacity}, "{section} size")'
            )
        layout.append(
            'ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "unexpected runtime data")'
        )
        (work / "layout.ld").write_text("\n".join(layout) + "\n")
        objects = []
        for source in ("reset_speed.S", "reset_speed_core.S", "uart_parser.c"):
            output = work / (source + ".o")
            cmd = [
                clang,
                "--target=arm-none-eabi",
                "-mcpu=cortex-m0",
                "-mthumb",
                "-Oz",
                "-ffreestanding",
                "-fno-builtin",
                "-fno-unwind-tables",
                "-fno-asynchronous-unwind-tables",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-c",
                str(ROOT / "tools/patches" / source),
                "-o",
                str(output),
            ]
            subprocess.run(cmd, check=True, capture_output=True)
            objects.append(str(output))
        elf = work / "patch.elf"
        subprocess.run(
            [linker, "-T", str(work / "layout.ld"), *objects, "-o", str(elf)],
            check=True,
            capture_output=True,
        )
        sections = {}
        for section in SECTIONS:
            out = work / (section + ".bin")
            subprocess.run(
                [
                    objcopy,
                    "-O",
                    "binary",
                    "--only-section=." + section,
                    str(elf),
                    str(out),
                ],
                check=True,
                capture_output=True,
            )
            sections[section] = out.read_bytes()
        symbol_text = subprocess.run(
            [nm, "--defined-only", "--numeric-sort", str(elf)],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        symbols = {
            fields[2]: int(fields[0], 16) & ~1
            for line in symbol_text.splitlines()
            if len(fields := line.split()) == 3 and fields[2].startswith("rs_")
        }
        return sections, symbols


def identify(data):
    """Return the flash offset of an exact saved CL01 full or application image.

    Raise ImageError when the image size and digest do not match a supported input.
    """
    if len(data) == FULL_SIZE and sha256(data) == SOURCE_FULL_SHA256:
        return 0
    if len(data) == APP_SIZE and sha256(data) == SOURCE_APP_SHA256:
        return APP_OFFSET
    raise ImageError(
        "Input must be the exact saved DT02+CL01 R3 full/application image"
    )


def build(data, llvm_bin=None):
    """Build RS01 from exact CL01 bytes and return its verified image and manifest.

    Require intact hooks and an erased RS01 reservation, confine changes to
    declared spans, and verify the output digest and erased tail after the
    install payload.
    """
    origin = identify(data)
    sections, symbols = compile_patch(llvm_bin)
    original_hooks = {
        0x283C: bytes.fromhex("c1730008"),
        0x44F4: bytes.fromhex("f0b58c4601790468"),
        0x7636: bytes.fromhex("00f0a9fc"),
        0x8688: bytes.fromhex("fdf7e6f9"),
        0x5C0C: bytes.fromhex("fff760fe"),
        0x63C8: bytes.fromhex("216a6068"),
        0x70D8: bytes.fromhex("10b5074c0022064b"),
    }
    cl_sections, _ = compile_cl01(llvm_bin)
    original_hooks[0x85F8] = cl_sections["boot_hook"]
    original_hooks[0x7BAE] = cl_sections["hook"]
    for offset, expected in original_hooks.items():
        if data[offset - origin : offset - origin + len(expected)] != expected:
            raise ImageError(f"Unexpected source hook {FLASH_BASE + offset:#x}")
    if data[0x9400 - origin : 0xA800 - origin] != b"\xff" * 0x1400:
        raise ImageError("RS01 flash reservation is not erased")
    output = bytearray(data)
    spans = []
    for name, replacement in sections.items():
        offset = SECTIONS[name][0] - origin
        output[offset : offset + len(replacement)] = replacement
        spans.append(
            dict(
                section=name,
                address=FLASH_BASE + SECTIONS[name][0],
                image_offset=offset,
                size=len(replacement),
                replacement_sha256=sha256(replacement),
            )
        )
    result = bytes(output)
    allowed = {s["image_offset"] + i for s in spans for i in range(s["size"])}
    if len(result) != len(data) or any(
        a != b and i not in allowed for i, (a, b) in enumerate(zip(data, result))
    ):
        raise ImageError("Output changed bytes outside declared spans")
    pin = OUTPUT_FULL_SHA256 if origin == 0 else OUTPUT_APP_SHA256
    if sha256(result) != pin:
        raise ImageError("Compiled output differs from pinned RS01 hash")
    if result[APP_OFFSET - origin + PAYLOAD_SIZE :] != b"\xff" * (
        APP_SIZE - PAYLOAD_SIZE
    ):
        raise ImageError("Non-erased bytes after RS01 install payload")
    manifest = dict(
        schema_version=1,
        patch_id="t150-dt02-cl01-reset-speed-rs01",
        candidate_revision=7,
        offline_only=True,
        hardware_tested=False,
        installable_approved=False,
        source_sha256=sha256(data),
        source_size=len(data),
        output_sha256=sha256(result),
        output_size=len(result),
        output_application_sha256=sha256(result[APP_OFFSET - origin :]),
        image_origin=origin,
        payload_size=PAYLOAD_SIZE,
        patch_spans=spans,
        symbols=symbols,
        ram=dict(
            start=0x20000FD0,
            state_end_exclusive=0x20000FEC,
            state_size=28,
            parser_start=0x20000FEC,
            parser_end_exclusive=0x20000FF0,
            parser_size=4,
            end_exclusive=0x20001000,
        ),
        lease_ticks=1500,
        source_files={
            str(p.relative_to(ROOT)): sha256(p.read_bytes())
            for p in (
                ROOT / "tools/patches/reset_speed.S",
                ROOT / "tools/patches/reset_speed.c",
                ROOT / "tools/patches/reset_speed_core.S",
                ROOT / "tools/patches/uart_parser.c",
                Path(__file__).resolve(),
            )
        },
    )
    return result, manifest


def main():
    """Write RS01 and its manifest to new files, reporting input or build failures.

    Remove the new image if its manifest cannot be written.
    """
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    p.add_argument("output", type=Path)
    p.add_argument("--manifest", required=True, type=Path)
    p.add_argument("--llvm-bin", type=Path)
    a = p.parse_args()
    for path in (a.output, a.manifest):
        if path.exists() or not path.parent.is_dir():
            p.error(f"Output must be new under an existing directory: {path}")
    try:
        result, manifest = build(read_saved_image(a.source), a.llvm_bin)
    except (ImageError, OSError, subprocess.CalledProcessError) as e:
        p.error(
            e.stderr.decode()
            if isinstance(e, subprocess.CalledProcessError)
            and isinstance(e.stderr, bytes)
            else str(e)
        )
    manifest["reproduce"] = [
        "python3",
        "tools/build_reset_speed_patch.py",
        "<saved-CL01R3-image>",
        "<new-output>",
        "--manifest",
        "<new-manifest>",
        "--llvm-bin",
        "<LLVM-bin>",
    ]
    with a.output.open("xb") as f:
        f.write(result)
    try:
        with a.manifest.open("x") as f:
            f.write(json.dumps(manifest, indent=2) + "\n")
    except BaseException:
        a.output.unlink()
        raise
    print(
        json.dumps(
            {
                k: manifest[k]
                for k in (
                    "patch_id",
                    "output_sha256",
                    "payload_size",
                    "hardware_tested",
                )
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
