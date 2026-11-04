#!/usr/bin/env python3
"""Verify trim image boundaries with synthetic anchors and optional exact R7 input."""

import argparse
import copy
from pathlib import Path
import unittest

import assemble_motion_trim as assembly
from build_child_lock_patch import ImageError, sha256
import build_platform_patch as platform

NATIVE_IMAGE = None
LLVM_BIN = None


def synthetic_source():
    """Create only the checked source anchors, without manufacturer firmware."""
    source = bytearray(b"\xff" * platform.FULL_SIZE)
    for offset, data in {**platform.SOURCE_ANCHORS, **platform.BOOT_ANCHORS}.items():
        source[offset : offset + len(data)] = data
    for offset, data in platform.HOOKS.values():
        source[offset : offset + len(data)] = data
    for name, (start, end) in assembly.TRIM_HOOKS.items():
        source[start:end] = bytes.fromhex(assembly.HOOK_ORIGINALS[name])
    return bytes(source)


class AssemblyTest(unittest.TestCase):
    """Require exact spans, reversible assembly and rejection of malformed input."""

    def setUp(self):
        """Use synthetic replacement bytes to test assembly independently of LLVM."""
        self.source = synthetic_source()
        self.sections = {
            name: b"\xa5" * len(original)
            for name, (_, original) in platform.HOOKS.items()
        }
        self.sections.update(
            {
                name: b"\x5a" * (end - start)
                for name, (start, end) in assembly.TRIM_HOOKS.items()
            }
        )
        self.sections["candidate"] = b"\x00\xbf"
        self.symbols = {"__aeabi_uidiv": 0x080028F5}

    def test_full_and_application_inverse(self):
        """Full and application inputs produce matching bodies and reversible spans."""
        outputs = []
        for origin in (0, platform.APP_OFFSET):
            source = self.source[origin:]
            output, spans = assembly._apply_sections(
                source, origin, self.sections, self.symbols
            )
            self.assertEqual(len(output), len(source))
            self.assertEqual(
                output[platform.PAYLOAD_END - origin :],
                source[platform.PAYLOAD_END - origin :],
            )
            if not origin:
                self.assertEqual(
                    output[: platform.APP_OFFSET], source[: platform.APP_OFFSET]
                )
            offset = platform.LAYOUT["descriptor_offset"] - origin
            self.assertEqual(output[offset : offset + 8], b"T150TR01")
            recovered = bytearray(output)
            for span in spans:
                start = span["image_offset"]
                self.assertEqual(
                    output[start : start + span["size"]], bytes.fromhex(span["after"])
                )
                recovered[start : start + span["size"]] = bytes.fromhex(span["before"])
            self.assertEqual(bytes(recovered), source)
            outputs.append(output[platform.APP_OFFSET - origin :])
        self.assertEqual(outputs[0], outputs[1])

    def test_each_source_hook_and_replacement_size(self):
        """Every trim hook rejects altered source bytes and truncated replacements."""
        for name, (start, _) in assembly.TRIM_HOOKS.items():
            with self.subTest(hook=name):
                source = bytearray(self.source)
                source[start] ^= 1
                with self.assertRaises(ImageError):
                    assembly._apply_sections(
                        bytes(source), 0, self.sections, self.symbols
                    )
                sections = copy.copy(self.sections)
                sections[name] = sections[name][:-1]
                with self.assertRaises(ImageError):
                    assembly._apply_sections(self.source, 0, sections, self.symbols)

    def test_section_set_and_capacity(self):
        """Unknown or missing sections and oversized code cannot form an image."""
        for mutation in (
            lambda s: s.update(unknown=b"\0"),
            lambda s: s.pop("hook_trim_gpio"),
            lambda s: s.update(candidate=b"\0" * (platform.CODE_CAPACITY + 1)),
        ):
            sections = copy.copy(self.sections)
            mutation(sections)
            with self.assertRaises(ImageError):
                assembly._apply_sections(self.source, 0, sections, self.symbols)

    def test_reserved_region_and_native_binding(self):
        """Occupied high flash and a changed native division binding are rejected."""
        for offset in (platform.CODE_OFFSET, platform.PAYLOAD_END):
            source = bytearray(self.source)
            source[offset] = 0
            with self.assertRaises(ImageError):
                assembly._apply_sections(bytes(source), 0, self.sections, self.symbols)
        with self.assertRaises(ImageError):
            assembly._apply_sections(
                self.source, 0, self.sections, {"__aeabi_uidiv": 0x080028F7}
            )

    def test_public_entry_rejects_synthetic_source(self):
        """Synthetic anchors cannot satisfy exact-source identity admission."""
        with self.assertRaises(ImageError):
            assembly.assemble(self.source, boot_profile={})


class ExactAssemblyTest(unittest.TestCase):
    """Bind actual linked analysis code to a private exact R7 source image."""

    def test_exact_r7_assembly(self):
        """Known R7 accepts every hook while preserving loader and protected bytes."""
        if NATIVE_IMAGE is None:
            self.skipTest("Pass --native-image for private exact R7 assembly")
        source = NATIVE_IMAGE.read_bytes()
        self.assertEqual(sha256(source), platform.SOURCE_FULL_SHA256)
        profile = dict(
            timing=0x10420F13,
            transfer_ticks=3,
            write_ticks=5,
            poll_ticks=1,
            stop_ticks=1,
            max_poll_gap=1,
            address=0x50,
            record_address=0x62,
            delivery_ticks=2,
        )
        with self.assertRaises(ImageError):
            assembly.assemble(source, boot_profile=None, llvm_bin=LLVM_BIN)
        output, manifest = assembly.assemble(
            source, boot_profile=profile, llvm_bin=LLVM_BIN
        )
        self.assertFalse(manifest["installable"])
        self.assertFalse(manifest["build"]["installable"])
        self.assertEqual(manifest["output_sha256"], sha256(output))
        self.assertEqual(output[: platform.APP_OFFSET], source[: platform.APP_OFFSET])
        self.assertEqual(output[platform.PAYLOAD_END :], source[platform.PAYLOAD_END :])
        self.assertNotEqual(manifest["application_sha256"], platform.OUTPUT_APP_SHA256)
        recovered = bytearray(output)
        for span in manifest["spans"]:
            start = span["image_offset"]
            recovered[start : start + span["size"]] = bytes.fromhex(span["before"])
        self.assertEqual(bytes(recovered), source)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-image", type=Path)
    parser.add_argument("--llvm-bin", type=Path)
    arguments, remaining = parser.parse_known_args()
    NATIVE_IMAGE, LLVM_BIN = arguments.native_image, arguments.llvm_bin
    unittest.main(argv=[__file__, *remaining])
