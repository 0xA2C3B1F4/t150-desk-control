#!/usr/bin/env python3
"""Build the fixed DS01 controller profile with display timeout from an exact saved R7 image."""

from pathlib import Path
import re

import assemble_motion_trim as assembly
from build_child_lock_patch import ImageError, sha256
from build_motion_trim_patch import BOOT_PROFILE, write_outputs as write_outputs
import build_platform_patch as platform
import compile_motion_trim as compiler

OUTPUT_FULL_SHA256 = "5fe2bd41c0edd49402b5332fa77c8f2e986bc13e500080a573db0e30a3568ec5"
OUTPUT_APP_SHA256 = "3ecdedb88e63c6b83bb2b6816dbb8bb4003ead092f4430b405ba5706a4a6c5b9"
OUTPUT_PAYLOAD_SHA256 = (
    "7cf4413dbc333bf7978d156a1721197fe60b71329724f19b4e696d1d5b744561"
)
PATCH_SECTIONS_SHA256 = (
    "2d2b48ebbb7d25d4067f3802f5685780f6cc27acb95d6ebb0a33c55cf5ef9173"
)
PAYLOAD_SIZE = platform.PAYLOAD_SIZE
HOOKS = {
    "hook_buzzer_timer": (0x67EC, bytes.fromhex("70b53c21fcf780f8")),
    "hook_display_stop": (0x7D76, bytes.fromhex("d0780128")),
    "hook_display_render": (0x8678, bytes.fromhex("fbf750fc")),
}
SECTION_ORDER = (
    *platform.HOOKS,
    *assembly.TRIM_HOOKS,
    *HOOKS,
    "candidate",
    "descriptor",
)


def descriptor():
    """Keep RAM ABI 1 and distinguish this controller image from BZ01."""
    return b"T150DS01" + platform.descriptor()[8:]


def sections_sha256(sections):
    """Bind the complete set of project-owned hooks and code in a fixed order."""
    if set(sections) != set(SECTION_ORDER):
        raise ImageError("Unexpected DS01 sections")
    return sha256(b"".join(sections[name] for name in SECTION_ORDER))


def compile_patch(llvm_bin=None):
    """Compile the fixed profile and require its pinned machine-code identity."""
    for name in (
        "OUTPUT_FULL_SHA256",
        "OUTPUT_APP_SHA256",
        "OUTPUT_PAYLOAD_SHA256",
        "PATCH_SECTIONS_SHA256",
    ):
        if not re.fullmatch(r"[0-9a-f]{64}", globals()[name]):
            raise ImageError(f"Invalid DS01 pin: {name}")
    platform.check_source()
    sections, symbols, provenance = compiler.compile_patch(
        llvm_bin, boot_profile=BOOT_PROFILE.copy(), buzzer=True, display=True
    )
    sections["descriptor"] = descriptor()
    if sections_sha256(sections) != PATCH_SECTIONS_SHA256:
        raise ImageError("DS01 sections differ from the pinned build")
    return sections, symbols, provenance


def apply_sections(source, origin, sections, symbols):
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
            raise ImageError("Unexpected DS01 hook source or length")
        if any(
            offset < span["offset"] + span["size"]
            and offset + len(data) > span["offset"]
            for span in spans
        ):
            raise ImageError("Overlapping DS01 hook")
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
            "DS01 inverse reconstruction or final-page preservation failed"
        )
    return bytes(output), spans


def build(data, llvm_bin=None, *, require_clean_source=False):
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
        raise ImageError("DS01 image differs from the pinned build")
    manifest = dict(
        patch="DS01",
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
                    "build_display_patch.py",
                    "compile_motion_trim.py",
                    "assemble_motion_trim.py",
                    "build_platform_patch.py",
                )
            },
        },
        validation="docs/validation.md#display-timeout",
        **source,
    )
    return output, manifest
