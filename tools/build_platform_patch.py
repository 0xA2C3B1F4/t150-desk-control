#!/usr/bin/env python3
"""Build the C8-only FP01 shared platform from an exact saved RS01 R7 image."""

import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

from build_child_lock_patch import (
    APP_OFFSET,
    APP_SIZE,
    FLASH_BASE,
    FULL_SIZE,
    ROOT,
    ImageError,
    find_tool,
    read_saved_image,
    sha256,
)
from build_reset_speed_patch import (
    OUTPUT_APP_SHA256 as SOURCE_APP_SHA256,
    OUTPUT_FULL_SHA256 as SOURCE_FULL_SHA256,
)
from build_tb01_patch import HOOKS as TB01_HOOKS
from generate_platform_layout import check_source, load_layout

PATCHES = ROOT / "tools/patches"
LAYOUT = load_layout()
CODE_OFFSET = LAYOUT["code_offset"]
CODE_CAPACITY = LAYOUT["code_end"] - CODE_OFFSET
PAYLOAD_SIZE = LAYOUT["payload_size"]
PAYLOAD_END = LAYOUT["payload_end"]
HOOKS = {**TB01_HOOKS, "hook_boot": (0x85F4, bytes.fromhex("fcf750fa"))}
SECTION_ORDER = (*HOOKS, "candidate", "descriptor")
# Pinned LLVM 23.1.0 output; physical acceptance is recorded separately.
OUTPUT_FULL_SHA256 = "6a2ac33ba77713c9de9d675be4e2629af229e4110f78d7c35defbb1d13555992"
OUTPUT_APP_SHA256 = "7ae194172863a9cff9847b671bfe24a1d33399d5fa3a90eabb158992f70a0adb"
OUTPUT_PAYLOAD_SHA256 = (
    "758fd91386471d3be09bffd4af86b7eb307f8158d33af62b33fe1909ca4ff144"
)
PATCH_SECTIONS_SHA256 = (
    "f6a8b4aa14b749e9bf67579da72a38f21188094434ad1c06b9d41a44d6f525d0"
)
SOURCE_FILES = (
    "platform_layout.json",
    "platform_layout.h",
    "platform_layout.ld",
    "platform_runtime.h",
    "platform_runtime.c",
    "platform_tb01.c",
    "platform_boot.S",
    "platform_patch.ld",
    "tb01.c",
    "tb01_core.c",
    "tb01_core.h",
    "tb01_hooks.S",
)
# The exact source digest is primary. These anchors explain the RAM/boot binding
# and fail independently when the apply routine is tested with synthetic data.
SOURCE_ANCHORS = {
    0x2800: bytes.fromhex("c80f0020d9280008"),
    0x28C0: bytes.fromhex("0448854600f056fc00480047f5850008c046c046c80f0020"),
    0x8718: bytes.fromhex(
        "38870008c0000020d4000000848200080c88000894010020340e000094820008"
    ),
    0x34D0: bytes.fromhex("a8080020"),
}
BOOT_ANCHORS = {
    0x0000: bytes.fromhex("080e0020"),
    0x00C0: bytes.fromhex("0348854600f03cf80048004701160008080e0020"),
    0x1620: bytes.fromhex(
        "40160008c000002038000000b615000878160008f8000020100d0000c6150008"
    ),
}


def descriptor():
    """Encode the reviewed 32-byte descriptor with full absolute addresses."""
    return struct.pack(
        "<8s6I",
        LAYOUT["descriptor_magic"].encode(),
        LAYOUT["layout_version"],
        FLASH_BASE + CODE_OFFSET,
        FLASH_BASE + LAYOUT["code_end"],
        LAYOUT["ram_start"],
        LAYOUT["ram_size"],
        LAYOUT["ram_abi"],
    )


def sections_sha256(sections):
    """Hash project-owned sections in their documented stable order."""
    if set(sections) != set(SECTION_ORDER):
        raise ImageError("Unexpected FP01 sections")
    return sha256(b"".join(sections[name] for name in SECTION_ORDER))


def _validate_hash_pins():
    for name in (
        "OUTPUT_FULL_SHA256",
        "OUTPUT_APP_SHA256",
        "OUTPUT_PAYLOAD_SHA256",
        "PATCH_SECTIONS_SHA256",
    ):
        pin = globals()[name]
        if not isinstance(pin, str) or not re.fullmatch(r"[0-9a-f]{64}", pin):
            raise ImageError(f"{name} must be a nonempty lowercase SHA-256 pin")


def _source_provenance(require_clean_source=False):
    commit = subprocess.run(
        ["git", "-C", str(ROOT), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    dirty = bool(
        subprocess.run(
            [
                "git",
                "-C",
                str(ROOT),
                "status",
                "--porcelain=v1",
                "--untracked-files=all",
            ],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    )
    if require_clean_source and dirty:
        raise ImageError("Release builds require a clean committed source checkout")
    return dict(source_commit=commit, source_dirty=dirty)


def _compile_patch(llvm_bin):
    _validate_hash_pins()
    check_source()
    task_tmp = os.environ.get("TMPDIR")
    if not task_tmp or not Path(task_tmp).is_dir() or not os.access(task_tmp, os.W_OK):
        raise ImageError("TMPDIR must name an existing writable temporary directory")
    executables = {
        name: find_tool(name, llvm_bin)
        for name in ("clang", "ld.lld", "llvm-objcopy", "llvm-nm")
    }
    toolchain = {}
    for name, executable in executables.items():
        version = subprocess.run(
            [executable, "--version"], check=True, capture_output=True, text=True
        ).stdout
        if not re.search(r"\b23\.1\.0\b", version):
            raise ImageError("FP01 requires the pinned LLVM 23.1.0 toolchain")
        toolchain[name] = dict(path=executable, version=version.strip())
    commands = []
    with tempfile.TemporaryDirectory(
        prefix="t150-fp01-build-", dir=task_tmp
    ) as directory:
        work = Path(directory)

        def run(command, text=False):
            commands.append(
                [
                    str(a)
                    .replace(str(work), "$BUILD_DIR")
                    .replace(str(PATCHES), "$SOURCE_DIR")
                    for a in command
                ]
            )
            return subprocess.run(command, check=True, capture_output=True, text=text)

        objects = []
        frame_reports = {}
        for source in (
            "tb01_core.c",
            "platform_runtime.c",
            "platform_tb01.c",
            "tb01_hooks.S",
            "platform_boot.S",
        ):
            obj = work / (source + ".o")
            flags = ["--target=arm-none-eabi", "-mcpu=cortex-m0", "-mthumb"]
            if source.endswith(".c"):
                flags += [
                    "-std=c11",
                    "-Oz",
                    "-ffreestanding",
                    "-fno-builtin",
                    "-fno-unwind-tables",
                    "-fno-asynchronous-unwind-tables",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fstack-usage",
                ]
            run(
                [
                    executables["clang"],
                    *flags,
                    "-c",
                    str(PATCHES / source),
                    "-o",
                    str(obj),
                ]
            )
            objects.append(str(obj))
            report = obj.with_suffix(".su")
            if report.exists():
                frame_reports[source] = report.read_text().replace(
                    str(PATCHES) + "/", ""
                )
        elf = work / "platform.elf"
        run(
            [
                executables["ld.lld"],
                "-L",
                str(PATCHES),
                "-T",
                str(PATCHES / "platform_patch.ld"),
                *objects,
                "-o",
                str(elf),
            ]
        )
        sections = {"descriptor": descriptor()}
        for name in SECTION_ORDER[:-1]:
            output = work / (name + ".bin")
            run(
                [
                    executables["llvm-objcopy"],
                    "-O",
                    "binary",
                    "--only-section=." + name,
                    str(elf),
                    str(output),
                ]
            )
            sections[name] = output.read_bytes()
        symbol_text = run(
            [executables["llvm-nm"], "--defined-only", "--numeric-sort", str(elf)],
            text=True,
        ).stdout
        symbols = {
            fields[2]: int(fields[0], 16) & (~1 if fields[1].lower() == "t" else -1)
            for line in symbol_text.splitlines()
            if len(fields := line.split()) == 3
        }
    if sections_sha256(sections) != PATCH_SECTIONS_SHA256:
        raise ImageError("FP01 sections differ from the pinned LLVM 23.1.0 build")
    toolchain["commands"] = commands
    toolchain["compiler_frames"] = frame_reports
    return sections, symbols, toolchain


def compile_patch(llvm_bin=None):
    """Compile project-owned sections and return their absolute symbol map."""
    sections, symbols, _ = _compile_patch(llvm_bin)
    return sections, symbols


def identify(data):
    """Accept only exact saved R7 full/application input images."""
    if len(data) == FULL_SIZE and sha256(data) == SOURCE_FULL_SHA256:
        return 0
    if len(data) == APP_SIZE and sha256(data) == SOURCE_APP_SHA256:
        return APP_OFFSET
    raise ImageError("Input must be the exact saved RS01 R7 full/application image")


def _apply_sections(source, origin, sections, symbols):
    if origin not in (0, APP_OFFSET) or len(source) != FULL_SIZE - origin:
        raise ImageError("Unexpected source image layout")
    if set(sections) != set(SECTION_ORDER):
        raise ImageError("Unexpected FP01 sections")
    for offset, expected in {
        **SOURCE_ANCHORS,
        **(BOOT_ANCHORS if origin == 0 else {}),
    }.items():
        if source[offset - origin : offset - origin + len(expected)] != expected:
            raise ImageError(f"Unexpected source boot/RAM anchor {offset:#x}")
    for name, (offset, expected) in HOOKS.items():
        if source[offset - origin : offset - origin + len(expected)] != expected or len(
            sections[name]
        ) != len(expected):
            raise ImageError(f"Unexpected source hook or hook size {name}")
    if symbols.get("__aeabi_uidiv") != 0x080028F5:
        raise ImageError("Native division binding changed")
    if not sections["candidate"] or len(sections["candidate"]) > CODE_CAPACITY:
        raise ImageError("FP01 code exceeds shared flash reservation")
    if sections["descriptor"] != descriptor():
        raise ImageError("Unexpected FP01 descriptor")
    if source[CODE_OFFSET - origin :] != b"\xff" * (FULL_SIZE - CODE_OFFSET):
        raise ImageError(
            "Shared flash reservation or protected final page is not erased"
        )
    offsets = {name: offset for name, (offset, _) in HOOKS.items()}
    offsets.update(candidate=CODE_OFFSET, descriptor=LAYOUT["descriptor_offset"])
    output, spans, used = bytearray(source), [], set()
    for name in SECTION_ORDER:
        replacement, offset = sections[name], offsets[name]
        addresses = set(range(offset, offset + len(replacement)))
        if (
            offset < APP_OFFSET
            or offset + len(replacement) > PAYLOAD_END
            or used & addresses
        ):
            raise ImageError("FP01 sections overlap or leave the payload")
        used.update(addresses)
        image_offset = offset - origin
        before = source[image_offset : image_offset + len(replacement)]
        output[image_offset : image_offset + len(replacement)] = replacement
        spans.append(
            dict(
                name=name,
                offset=offset,
                image_offset=image_offset,
                size=len(replacement),
                before=before.hex(),
                after=replacement.hex(),
            )
        )
    recovered = bytearray(output)
    for span in spans:
        offset, before = span["image_offset"], bytes.fromhex(span["before"])
        recovered[offset : offset + len(before)] = before
    if (
        bytes(recovered) != source
        or output[PAYLOAD_END - origin :] != source[PAYLOAD_END - origin :]
        or (origin == 0 and output[:APP_OFFSET] != source[:APP_OFFSET])
    ):
        raise ImageError("Inverse, bootloader or protected-page check failed")
    return bytes(output), spans


def build(data, llvm_bin=None, *, require_clean_source=False):
    """Build a profile-bound image with source, layout and toolchain provenance."""
    _validate_hash_pins()
    origin = identify(data)
    provenance = _source_provenance(require_clean_source)
    sections, symbols, toolchain = _compile_patch(llvm_bin)
    output, spans = _apply_sections(data, origin, sections, symbols)
    app = output[APP_OFFSET - origin :]
    payload = output[APP_OFFSET - origin : PAYLOAD_END - origin]
    for expected, actual in (
        (OUTPUT_FULL_SHA256 if origin == 0 else OUTPUT_APP_SHA256, sha256(output)),
        (OUTPUT_PAYLOAD_SHA256, sha256(payload)),
    ):
        if expected != actual:
            raise ImageError("FP01 output differs from the pinned build")
    manifest = dict(
        patch="FP01",
        source_sha256=sha256(data),
        output_sha256=sha256(output),
        application_sha256=sha256(app),
        application_size=APP_SIZE,
        payload_sha256=sha256(payload),
        install_payload_offset=APP_OFFSET,
        install_payload_size=PAYLOAD_SIZE,
        code_size=len(sections["candidate"]),
        patch_sections_sha256=sections_sha256(sections),
        layout=LAYOUT,
        initial_msp="0x20000fc8",
        ram_start="0x20001000",
        ram_size=1024,
        installation_profile=LAYOUT["profile"],
        spans=spans,
        symbols=symbols,
        **provenance,
        source_files={
            **{name: sha256((PATCHES / name).read_bytes()) for name in SOURCE_FILES},
            **{
                name: sha256((ROOT / "tools" / name).read_bytes())
                for name in ("build_platform_patch.py", "generate_platform_layout.py")
            },
        },
        generation="clang 23.1.0; maintained common TB01 C core",
        toolchain=toolchain,
        limitations=[
            "Explicit photographed STM32F030C8T6 board and exact firmware profile only",
            "DBGMCU/flash-size pair is a rejection gate, not package authentication",
            "Physical upper-SRAM independence and cold/reset/sleep acceptance pending",
            "Arbitrary direct application jumps with inherited RAM vector remap unsupported",
            "Builder and emulation perform no hardware validation",
            "Native timer rounds seconds to zero; buzzer requires live SysTick",
        ],
    )
    return output, manifest


def _write_outputs(directory, output, origin, manifest):
    artifacts = {
        "t150-fp01.application.bin": output[APP_OFFSET - origin :],
        "t150-fp01.payload.bin": output[APP_OFFSET - origin : PAYLOAD_END - origin],
        "manifest.json": (json.dumps(manifest, indent=2) + "\n").encode(),
    }
    if origin == 0:
        artifacts["t150-fp01.full.bin"] = output
    directory.mkdir()
    created = []
    try:
        for name, data in artifacts.items():
            path = directory / name
            with path.open("xb") as file:
                created.append(path)
                file.write(data)
    except BaseException:
        for path in reversed(created):
            path.unlink()
        directory.rmdir()
        raise


def main():
    """Build offline artifacts in a new directory without opening a device."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--llvm-bin", type=Path)
    parser.add_argument(
        "--require-clean-source",
        action="store_true",
        help="Require committed, clean source for release artifacts",
    )
    args = parser.parse_args()
    try:
        data = read_saved_image(args.source)
        output, manifest = build(
            data, args.llvm_bin, require_clean_source=args.require_clean_source
        )
        _write_outputs(args.output_directory, output, identify(data), manifest)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
    print(
        json.dumps(
            {
                key: manifest[key]
                for key in ("output_sha256", "application_sha256", "code_size")
            }
        )
    )


if __name__ == "__main__":
    main()
