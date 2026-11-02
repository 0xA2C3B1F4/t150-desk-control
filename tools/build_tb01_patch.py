#!/usr/bin/env python3
"""Build TB01 offline from an exact saved RS01 R7 full or application image."""

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
from generate_tb01_core import check_source

PATCHES = ROOT / "tools/patches"
CODE_OFFSET = 0xA010
CODE_CAPACITY = 0x7F0
PAYLOAD_SIZE = 32768
PAYLOAD_END = APP_OFFSET + PAYLOAD_SIZE
HOOKS = {
    "hook_sound": (0x3408, bytes.fromhex("f8b5314d04460f46")),
    "hook_sleep": (0x865E, bytes.fromhex("fef783ff")),
    "hook_init": (0x9402, bytes.fromhex("00f04df8")),
    "hook_route": (0x9434, bytes.fromhex("00f0d4f9")),
    "hook_tick": (0x9452, bytes.fromhex("00f035f8")),
}
# These identify the pinned LLVM 23.1.0 build; hardware acceptance is recorded separately.
OUTPUT_FULL_SHA256 = "d9154fc156515d9d2d169b73130ee6ab71bd21061f2e9ac5fb2107735ffb4f2a"
OUTPUT_APP_SHA256 = "eb4ea2ff574aab412d5b7c5b4d6a74a3515ab6329d5459fae2143baec2356f07"
OUTPUT_PAYLOAD_SHA256 = (
    "7c9bed70d455b436e0712b15a4eb5e9bf43dbe666199bfac1edc537264013118"
)
# Concatenate these sections in this order, without vendor bytes or alignment gaps.
SECTION_ORDER = (*HOOKS, "candidate")
PATCH_SECTIONS_SHA256 = (
    "17508c66c8b1f89f2b7c7f21068d0b2c64b9e562118815580ffd7af84dfccdd1"
)
SOURCE_FILES = (
    "tb01_core.c",
    "tb01_core.h",
    "tb01.c",
    "tb01_core.S",
    "tb01_platform.S",
    "tb01_hooks.S",
    "tb01.ld",
)


def sections_sha256(sections):
    """Hash the six project-owned sections in the fixed SECTION_ORDER sequence."""
    if set(sections) != set(SECTION_ORDER):
        raise ImageError("Unexpected TB01 sections")
    return sha256(b"".join(sections[name] for name in SECTION_ORDER))


def _check_section_identity(sections):
    if sections_sha256(sections) != PATCH_SECTIONS_SHA256:
        raise ImageError("TB01 sections differ from the pinned LLVM 23.1.0 build")


def _compile_patch(llvm_bin):
    check_source()
    task_tmp = os.environ.get("TMPDIR")
    if not task_tmp or not Path(task_tmp).is_dir() or not os.access(task_tmp, os.W_OK):
        raise ImageError("TMPDIR must name an existing writable temporary directory")
    clang = find_tool("clang", llvm_bin)
    linker = find_tool("ld.lld", llvm_bin)
    objcopy = find_tool("llvm-objcopy", llvm_bin)
    nm = find_tool("llvm-nm", llvm_bin)
    toolchain = {
        name: dict(
            path=executable,
            version=subprocess.run(
                [executable, "--version"], check=True, capture_output=True, text=True
            ).stdout.splitlines()[0],
        )
        for name, executable in (
            ("clang", clang),
            ("ld.lld", linker),
            ("llvm-objcopy", objcopy),
            ("llvm-nm", nm),
        )
    }
    commands = []
    with tempfile.TemporaryDirectory(
        prefix="t150-tb01-build-", dir=task_tmp
    ) as directory:
        work = Path(directory)

        def record(command):
            commands.append(
                [
                    argument.replace(str(work), "$BUILD_DIR").replace(
                        str(PATCHES), "$SOURCE_DIR"
                    )
                    for argument in command
                ]
            )
            return command

        objects = []
        for source in ("tb01_core.S", "tb01_platform.S", "tb01_hooks.S"):
            obj = work / (source + ".o")
            subprocess.run(
                record(
                    [
                        clang,
                        "--target=arm-none-eabi",
                        "-mcpu=cortex-m0",
                        "-mthumb",
                        "-c",
                        str(PATCHES / source),
                        "-o",
                        str(obj),
                    ]
                ),
                check=True,
                capture_output=True,
            )
            objects.append(str(obj))
        elf = work / "tb01.elf"
        subprocess.run(
            record([linker, "-T", str(PATCHES / "tb01.ld"), *objects, "-o", str(elf)]),
            check=True,
            capture_output=True,
        )
        sections = {}
        for section in SECTION_ORDER:
            output = work / (section + ".bin")
            subprocess.run(
                record(
                    [
                        objcopy,
                        "-O",
                        "binary",
                        "--only-section=." + section,
                        str(elf),
                        str(output),
                    ]
                ),
                check=True,
                capture_output=True,
            )
            sections[section] = output.read_bytes()
        text = subprocess.run(
            record([nm, "--defined-only", "--numeric-sort", str(elf)]),
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        symbols = {
            fields[2]: int(fields[0], 16) & ~1
            for line in text.splitlines()
            if len(fields := line.split()) == 3
        }
    _check_section_identity(sections)
    toolchain["commands"] = commands
    return sections, symbols, toolchain


def compile_patch(llvm_bin=None):
    """Assemble frozen TB01 sources and require their pinned section digest."""
    sections, symbols, _ = _compile_patch(llvm_bin)
    return sections, symbols


def identify(data):
    """Return the flash origin only for exact saved RS01 R7 full/application bytes."""
    if len(data) == FULL_SIZE and sha256(data) == SOURCE_FULL_SHA256:
        return 0
    if len(data) == APP_SIZE and sha256(data) == SOURCE_APP_SHA256:
        return APP_OFFSET
    raise ImageError("Input must be the exact saved RS01 R7 full/application image")


def _apply_sections(source, origin, sections, symbols):
    if origin not in (0, APP_OFFSET) or len(source) != FULL_SIZE - origin:
        raise ImageError("Unexpected source image layout")
    if (
        int.from_bytes(source[APP_OFFSET - origin : APP_OFFSET - origin + 4], "little")
        != 0x20000FC8
    ):
        raise ImageError("Application MSP conflicts with the reserved patch RAM")
    if set(sections) != {*HOOKS, "candidate"}:
        raise ImageError("Unexpected TB01 sections")
    if (
        int.from_bytes(source[0x34D0 - origin : 0x34D4 - origin], "little")
        != 0x200008A8
    ):
        raise ImageError("Native sound-state literal differs from the reviewed binding")
    # identify() binds the complete native helper to exact reviewed R7 bytes.
    # This localized check verifies the linker binding, not the helper's semantics.
    if symbols.get("__aeabi_uidiv") != 0x080028F4:
        raise ImageError("Native unsigned-division binding changed")
    for name, (offset, expected) in HOOKS.items():
        if source[offset - origin : offset - origin + len(expected)] != expected:
            raise ImageError(f"Unexpected source hook {FLASH_BASE + offset:#x}")
        if len(sections[name]) != len(expected):
            raise ImageError(f"Unexpected hook size: {name}")
    code = sections["candidate"]
    if not code or len(code) > CODE_CAPACITY:
        raise ImageError("TB01 code exceeds its install-payload reservation")
    if source[CODE_OFFSET - origin : PAYLOAD_END - origin] != b"\xff" * CODE_CAPACITY:
        raise ImageError("TB01 flash reservation is not erased")
    offsets = {name: offset for name, (offset, _) in HOOKS.items()}
    offsets["candidate"] = CODE_OFFSET
    output, spans, used = bytearray(source), [], set()
    for name, replacement in sections.items():
        offset = offsets[name]
        addresses = set(range(offset, offset + len(replacement)))
        if (
            offset < APP_OFFSET
            or offset + len(replacement) > PAYLOAD_END
            or used & addresses
        ):
            raise ImageError("TB01 sections overlap or leave the install payload")
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
        offset = span["image_offset"]
        before = bytes.fromhex(span["before"])
        recovered[offset : offset + len(before)] = before
    if (
        bytes(recovered) != source
        or output[PAYLOAD_END - origin :] != source[PAYLOAD_END - origin :]
    ):
        raise ImageError("Inverse or install-payload boundary check failed")
    return bytes(output), spans


def build(data, llvm_bin=None):
    """Apply TB01 to exact R7 bytes and require the pinned full/app and payload hashes."""
    origin = identify(data)
    sections, symbols, toolchain = _compile_patch(llvm_bin)
    output, spans = _apply_sections(data, origin, sections, symbols)
    expected = OUTPUT_FULL_SHA256 if origin == 0 else OUTPUT_APP_SHA256
    payload = output[APP_OFFSET - origin : PAYLOAD_END - origin]
    if sha256(output) != expected or sha256(payload) != OUTPUT_PAYLOAD_SHA256:
        raise ImageError("TB01 output differs from the pinned LLVM 23.1.0 build")
    manifest = dict(
        patch="TB01",
        source_sha256=sha256(data),
        output_sha256=sha256(output),
        application_sha256=sha256(output[APP_OFFSET - origin :]),
        application_size=APP_SIZE,
        payload_sha256=sha256(payload),
        code_size=len(sections["candidate"]),
        patch_sections_sha256=sections_sha256(sections),
        install_payload_offset=APP_OFFSET,
        install_payload_size=PAYLOAD_SIZE,
        initial_msp="0x20000fc8",
        ram_start="0x20000ff0",
        ram_size=12,
        spans=spans,
        source_files={
            **{name: sha256((PATCHES / name).read_bytes()) for name in SOURCE_FILES},
            **{
                name: sha256((ROOT / "tools" / name).read_bytes())
                for name in ("build_tb01_patch.py", "generate_tb01_core.py")
            },
        },
        generation="clang 23.1.0; frozen assembly",
        toolchain=toolchain,
        limitations=[
            "Native timer rounds seconds to zero",
            "Builder performs no hardware validation",
            "Pulse-off requires live SysTick; no independent hardware bound",
        ],
    )
    return output, manifest


def _write_outputs(directory, output, origin, manifest):
    artifacts = {}
    if origin == 0:
        artifacts["t150-tb01.full.bin"] = output
    artifacts["t150-tb01.application.bin"] = output[APP_OFFSET - origin :]
    artifacts["t150-tb01.payload.bin"] = output[
        APP_OFFSET - origin : PAYLOAD_END - origin
    ]
    artifacts["manifest.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
    directory.mkdir()
    created = []
    try:
        for name, data in artifacts.items():
            path = directory / name
            with path.open("xb") as file:
                created.append(path)
                file.write(data)
    except BaseException as error:
        for path in reversed(created):
            try:
                path.unlink()
            except OSError as cleanup_error:
                error.add_note(
                    f"Could not remove partial output {path}: {cleanup_error}"
                )
        try:
            directory.rmdir()
        except OSError as cleanup_error:
            error.add_note(
                f"Could not remove output directory {directory}: {cleanup_error}"
            )
        raise


def main():
    """Read a bounded saved image with best-effort cleanup of failed output writes."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--llvm-bin", type=Path)
    args = parser.parse_args()
    try:
        source = read_saved_image(args.source)
        output, manifest = build(source, args.llvm_bin)
        origin = identify(source)
        _write_outputs(args.output_directory, output, origin, manifest)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        details = [str(error), *getattr(error, "__notes__", ())]
        parser.exit(1, "\n".join(details) + "\n")
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
