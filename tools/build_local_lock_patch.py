#!/usr/bin/env python3
"""Build the fixed LK01 controller profile with local panel lock from an exact saved R7 image."""

from pathlib import Path
import re

import assemble_motion_trim as assembly
from build_child_lock_patch import ImageError, sha256
import build_display_patch as display
from build_motion_trim_patch import BOOT_PROFILE, write_outputs as write_outputs
import build_platform_patch as platform
import compile_motion_trim as compiler

OUTPUT_FULL_SHA256 = "986bc06aeada5340e5b8832dbad6c0fa9ae0c1976fb86cfed8c4d1fcac892de6"
OUTPUT_APP_SHA256 = "040fa9a6de9b98124f90e9f631c26d3ec8860980a1c03f246b8f898e1b2aa6e1"
OUTPUT_PAYLOAD_SHA256 = (
    "1d67652e68ee628fc740f9a78618682499d71da631aa8e3bded3fb4af6fc4174"
)
PATCH_SECTIONS_SHA256 = (
    "3ec602fd14fc3ca80d1ff8521bd12ab47025c9e47c93e16b5a85544811039f2b"
)
PAYLOAD_SIZE = platform.PAYLOAD_SIZE
HOOKS = dict(display.HOOKS)
SECTION_ORDER = (
    *platform.HOOKS,
    *assembly.TRIM_HOOKS,
    *HOOKS,
    "candidate",
    "descriptor",
)


def descriptor() -> bytes:
    """Keep RAM ABI 1 and distinguish this controller image from DS01."""
    return b"T150LK01" + platform.descriptor()[8:]


def sections_sha256(sections: dict[str, bytes]) -> str:
    """Bind the complete set of project-owned hooks and code in a fixed order."""
    if set(sections) != set(SECTION_ORDER):
        raise ImageError("Unexpected LK01 sections")
    return sha256(b"".join(sections[name] for name in SECTION_ORDER))


def compile_patch(llvm_bin: Path | None = None) -> tuple:
    """Compile the fixed profile and require its pinned machine-code identity."""
    for name in (
        "OUTPUT_FULL_SHA256",
        "OUTPUT_APP_SHA256",
        "OUTPUT_PAYLOAD_SHA256",
        "PATCH_SECTIONS_SHA256",
    ):
        if not re.fullmatch(r"[0-9a-f]{64}", globals()[name]):
            raise ImageError(f"Invalid LK01 pin: {name}")
    platform.check_source()
    sections, symbols, provenance = compiler.compile_patch(
        llvm_bin,
        boot_profile=BOOT_PROFILE.copy(),
        buzzer=True,
        display=True,
        local_lock=True,
    )
    sections["descriptor"] = descriptor()
    if sections_sha256(sections) != PATCH_SECTIONS_SHA256:
        raise ImageError("LK01 sections differ from the pinned build")
    return sections, symbols, provenance


def apply_sections(
    source: bytes, origin: int, sections: dict[str, bytes], symbols: dict[str, int]
) -> tuple[bytes, list[dict]]:
    """Apply checked hooks and verify inverse reconstruction and protected ranges."""
    sections_sha256(sections)
    output, spans = assembly._apply_sections(
        source,
        origin,
        {
            name: data
            for name, data in sections.items()
            if name not in (*HOOKS, "descriptor")
        },
        symbols,
    )
    output = bytearray(output)
    for name, (offset, before) in HOOKS.items():
        data = sections[name]
        if source[offset - origin : offset - origin + len(before)] != before or len(
            data
        ) != len(before):
            raise ImageError("Unexpected LK01 hook source or length")
        if any(
            offset < span["offset"] + span["size"]
            and offset + len(data) > span["offset"]
            for span in spans
        ):
            raise ImageError("Overlapping LK01 hook")
        output[offset - origin : offset - origin + len(data)] = data
        spans.append(
            dict(
                name=name,
                offset=offset,
                image_offset=offset - origin,
                size=len(data),
                before=before.hex(),
                after=data.hex(),
            )
        )
    offset = platform.LAYOUT["descriptor_offset"] - origin
    output[offset : offset + 32] = descriptor()
    next(span for span in spans if span["name"] == "descriptor")["after"] = (
        descriptor().hex()
    )
    restored = bytearray(output)
    for span in spans:
        offset = span["image_offset"]
        restored[offset : offset + span["size"]] = bytes.fromhex(span["before"])
    if (
        restored != source
        or output[platform.PAYLOAD_END - origin :]
        != source[platform.PAYLOAD_END - origin :]
    ):
        raise ImageError(
            "LK01 inverse reconstruction or final-page preservation failed"
        )
    return bytes(output), spans


def build(
    data: bytes, llvm_bin: Path | None = None, *, require_clean_source: bool = False
) -> tuple[bytes, dict]:
    """Build the fixed controller with compiler, source and image provenance."""
    origin = platform.identify(data)
    source = platform._source_provenance(require_clean_source)
    sections, symbols, compilation = compile_patch(llvm_bin)
    output, spans = apply_sections(data, origin, sections, symbols)
    app = output[platform.APP_OFFSET - origin :]
    payload = output[platform.APP_OFFSET - origin : platform.PAYLOAD_END - origin]
    if (
        sha256(output) != (OUTPUT_FULL_SHA256 if origin == 0 else OUTPUT_APP_SHA256)
        or sha256(app) != OUTPUT_APP_SHA256
        or sha256(payload) != OUTPUT_PAYLOAD_SHA256
    ):
        raise ImageError("LK01 image differs from the pinned build")
    manifest = dict(
        patch="LK01",
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
                name: sha256(Path(__file__).with_name(name).read_bytes())
                for name in (
                    "build_local_lock_patch.py",
                    "build_display_patch.py",
                    "compile_motion_trim.py",
                    "assemble_motion_trim.py",
                    "build_platform_patch.py",
                )
            },
        },
        validation="docs/validation.md#local-panel-lock",
        **source,
    )
    return output, manifest
