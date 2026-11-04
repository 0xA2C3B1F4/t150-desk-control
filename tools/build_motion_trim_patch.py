#!/usr/bin/env python3
"""Build the fixed TR01 DT410 profile from an exact saved RS01 R7 image."""

import argparse
import json
from pathlib import Path
import re
import subprocess

import assemble_motion_trim as assembly
from build_child_lock_patch import ImageError, read_saved_image, sha256
import build_platform_patch as platform
import compile_motion_trim as compiler

OUTPUT_FULL_SHA256 = "d7ee37e8faa1754ce54a5d6230b1c8e1ff09a399a97bbc95dfb658f9732743f5"
OUTPUT_APP_SHA256 = "2493c4b1d24e1ed6b1e5fa478224a4466068d50b7d52781bc004e3b0d928dcc9"
OUTPUT_PAYLOAD_SHA256 = (
    "0dd15cb6cddcc3aae5a33cb6652fdf50aad7dfa36de33a449888c50fa0f22ca5"
)
PATCH_SECTIONS_SHA256 = (
    "ebabdf4288c2c18a0e7af4168cd7197c8f75b71d18f20530d388a26b5215053f"
)
BOOT_PROFILE = {
    "timing": 0x10722327,
    "transfer_ticks": 4,
    "write_ticks": 10,
    "poll_ticks": 1,
    "stop_ticks": 2,
    "max_poll_gap": 2,
    "address": 0x50,
    "record_address": 0x70,
    "delivery_ticks": 2,
}
SECTION_ORDER = (*platform.HOOKS, *assembly.TRIM_HOOKS, "candidate", "descriptor")
PAYLOAD_SIZE = platform.PAYLOAD_SIZE


def descriptor():
    """Preserve the installed TR01 identity and shared RAM layout."""
    return b"T150TR01" + platform.descriptor()[8:]


def sections_sha256(sections):
    """Bind every project-owned hook, code section and descriptor in fixed order."""
    if set(sections) != set(SECTION_ORDER):
        raise ImageError("Unexpected TR01 sections")
    return sha256(b"".join(sections[name] for name in SECTION_ORDER))


def validate_pins():
    """Reject missing image or section identities before compiling."""
    for name in (
        "OUTPUT_FULL_SHA256",
        "OUTPUT_APP_SHA256",
        "OUTPUT_PAYLOAD_SHA256",
        "PATCH_SECTIONS_SHA256",
    ):
        if not isinstance(globals()[name], str) or not re.fullmatch(
            r"[0-9a-f]{64}", globals()[name]
        ):
            raise ImageError(f"{name} must be a lowercase SHA-256 pin")


def compile_patch(llvm_bin=None):
    """Compile only the fixed tested profile and reject changed project bytes."""
    validate_pins()
    platform.check_source()
    sections, symbols, provenance = compiler.compile_patch(
        llvm_bin, boot_profile=BOOT_PROFILE.copy()
    )
    sections["descriptor"] = descriptor()
    if sections_sha256(sections) != PATCH_SECTIONS_SHA256:
        raise ImageError("TR01 sections differ from the pinned LLVM 23.1.0 build")
    return sections, symbols, provenance


def build(data, llvm_bin=None, *, require_clean_source=False):
    """Build the pinned image with source, profile and compiler provenance."""
    validate_pins()
    origin = platform.identify(data)
    source = platform._source_provenance(require_clean_source)
    sections, symbols, compilation = compile_patch(llvm_bin)
    output, spans = assembly._apply_sections(
        data, origin, {k: v for k, v in sections.items() if k != "descriptor"}, symbols
    )
    app = output[platform.APP_OFFSET - origin :]
    payload = output[platform.APP_OFFSET - origin : platform.PAYLOAD_END - origin]
    for actual, expected in (
        (sha256(output), OUTPUT_FULL_SHA256 if origin == 0 else OUTPUT_APP_SHA256),
        (sha256(app), OUTPUT_APP_SHA256),
        (sha256(payload), OUTPUT_PAYLOAD_SHA256),
    ):
        if actual != expected:
            raise ImageError("TR01 image differs from the pinned build")
    manifest = dict(
        patch="TR01",
        source_sha256=sha256(data),
        output_sha256=sha256(output),
        application_sha256=sha256(app),
        application_size=platform.APP_SIZE,
        payload_sha256=sha256(payload),
        install_payload_offset=platform.APP_OFFSET,
        install_payload_size=PAYLOAD_SIZE,
        installation_profile=platform.LAYOUT["profile"],
        layout=platform.LAYOUT,
        boot_profile=BOOT_PROFILE.copy(),
        patch_sections_sha256=sections_sha256(sections),
        code_size=len(sections["candidate"]),
        spans=spans,
        symbols=symbols,
        toolchain=compilation["toolchain"],
        commands=compilation["commands"],
        compiler_frames=compilation["frames"],
        source_files={
            **compilation["source_sha256"],
            **{
                name: sha256((platform.ROOT / "tools" / name).read_bytes())
                for name in (
                    "build_motion_trim_patch.py",
                    "assemble_motion_trim.py",
                    "compile_motion_trim.py",
                    "build_platform_patch.py",
                    "generate_platform_layout.py",
                )
            },
        },
        **source,
        validation="docs/validation.md#tr01-motion-trims",
    )
    return output, manifest


def write_outputs(directory, output, origin, manifest):
    """Write a new bundle, preserving existing paths and removing failed partial output."""
    artifacts = {
        "t150-tr01.application.bin": output[platform.APP_OFFSET - origin :],
        "t150-tr01.payload.bin": output[
            platform.APP_OFFSET - origin : platform.PAYLOAD_END - origin
        ],
        "manifest.json": (json.dumps(manifest, indent=2) + "\n").encode(),
    }
    if origin == 0:
        artifacts["t150-tr01.full.bin"] = output
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
    """Build offline files without opening a device or accepting timing overrides."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--llvm-bin", type=Path)
    parser.add_argument("--require-clean-source", action="store_true")
    args = parser.parse_args()
    try:
        data = read_saved_image(args.source)
        output, manifest = build(
            data, args.llvm_bin, require_clean_source=args.require_clean_source
        )
        write_outputs(args.output_directory, output, platform.identify(data), manifest)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
    print(
        json.dumps(
            {
                k: manifest[k]
                for k in ("output_sha256", "application_sha256", "code_size")
            }
        )
    )


if __name__ == "__main__":
    main()
