#!/usr/bin/env python3
"""Assemble a non-installable TR01 analysis image from an exact saved R7 image.

Explicit timing parameters permit offline instruction and stack analysis. They
do not qualify an EEPROM, authorize installation or add a supported image hash
to the maintenance installer.
"""

from pathlib import Path

from build_child_lock_patch import ImageError, sha256
import build_platform_patch as platform
from compile_motion_trim import STORAGE_HOOKS, TARGET_HOOKS, compile_patch

HOOK_ORIGINALS = {
    "hook_trim_preset_1": "49890a1d904201ddc01d03e0091f884201da801fe060",
    "hook_trim_preset_2": "49890a1d904201ddc01d03e0091f884201da801fe060",
    "hook_trim_preset_3": "49890a1d904201ddc01d03e0091f884201da801fe060",
    "hook_trim_direct": "009804990a1d904201dd073003e00439884200da0638",
    "hook_trim_tx": "f0b58c4609680b78",
    "hook_trim_gpio": "a98c0008",
    "hook_trim_native": "08b5012080f3108820480068",
    "hook_trim_uart": "01f03dfc",
    "hook_trim_startup": "1c4c1c4d",
    "hook_trim_selector": "21780029",
    "hook_trim_foreground": "fcf796fd",
}
TRIM_HOOKS = {**TARGET_HOOKS, **STORAGE_HOOKS}


def _apply_sections(source, origin, sections, symbols):
    expected_sections = {*platform.HOOKS, *TRIM_HOOKS, "candidate"}
    if set(sections) != expected_sections:
        raise ImageError("Unexpected trim analysis sections")
    base_sections = {name: sections[name] for name in platform.HOOKS}
    base_sections.update(
        candidate=sections["candidate"], descriptor=platform.descriptor()
    )
    base, spans = platform._apply_sections(source, origin, base_sections, symbols)
    output = bytearray(base)
    used = {
        address
        for span in spans
        for address in range(span["offset"], span["offset"] + span["size"])
    }
    for name, (start, end) in TRIM_HOOKS.items():
        original = bytes.fromhex(HOOK_ORIGINALS[name])
        replacement = sections[name]
        if (
            len(original) != end - start
            or len(replacement) != end - start
            or source[start - origin : end - origin] != original
        ):
            raise ImageError(f"Unexpected trim hook or replacement size: {name}")
        addresses = set(range(start, end))
        if (
            start < platform.APP_OFFSET
            or end > platform.PAYLOAD_END
            or used & addresses
        ):
            raise ImageError("Trim sections overlap or leave the payload")
        used.update(addresses)
        output[start - origin : end - origin] = replacement
        spans.append(
            dict(
                name=name,
                offset=start,
                image_offset=start - origin,
                size=len(replacement),
                before=original.hex(),
                after=replacement.hex(),
            )
        )
    # Keep the layout ABI but distinguish analysis output from the offered FP01.
    descriptor = b"T150TR01" + platform.descriptor()[8:]
    offset = platform.LAYOUT["descriptor_offset"] - origin
    output[offset : offset + len(descriptor)] = descriptor
    next(span for span in spans if span["name"] == "descriptor")["after"] = (
        descriptor.hex()
    )
    recovered = bytearray(output)
    for span in spans:
        offset = span["image_offset"]
        recovered[offset : offset + span["size"]] = bytes.fromhex(span["before"])
    if bytes(recovered) != source:
        raise ImageError("Trim inverse reconstruction failed")
    if (
        output[platform.PAYLOAD_END - origin :]
        != source[platform.PAYLOAD_END - origin :]
    ):
        raise ImageError("Trim changed the protected final page")
    if origin == 0 and output[: platform.APP_OFFSET] != source[: platform.APP_OFFSET]:
        raise ImageError("Trim changed the bootloader")
    return bytes(output), spans


def assemble(data, *, boot_profile, llvm_bin=None):
    """Return an exact-source analysis image and its non-installation provenance."""
    origin = platform.identify(data)
    if boot_profile is None:
        raise ImageError("Trim analysis requires explicit boot parameters")
    sections, symbols, build = compile_patch(llvm_bin, boot_profile=boot_profile)
    output, spans = _apply_sections(data, origin, sections, symbols)
    manifest = {
        "patch": "TR01-analysis",
        "installable": False,
        "source_sha256": sha256(data),
        "output_sha256": sha256(output),
        "application_sha256": sha256(output[platform.APP_OFFSET - origin :]),
        "origin": origin,
        "spans": spans,
        "symbols": symbols,
        "build": build,
        "assembly_sources": {
            name: sha256(Path(__file__).with_name(name).read_bytes())
            for name in (
                "assemble_motion_trim.py",
                "compile_motion_trim.py",
                "build_platform_patch.py",
            )
        },
        **platform._source_provenance(),
    }
    return output, manifest
