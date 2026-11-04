#!/usr/bin/env python3
"""Check BZ01 image admission, protected bytes and compiler identity offline."""

import argparse
from pathlib import Path
import unittest
from unittest.mock import patch

import build_buzzer_patch as builder
import build_platform_patch as platform
from test_motion_trim_builder import synthetic_source

LLVM_BIN = None


class BuzzerBuilderTests(unittest.TestCase):
    """Bind the new hooks and descriptor to exact, reversible image construction."""

    @classmethod
    def setUpClass(cls):
        """Compile once and construct a synthetic source with the native RTC hook."""
        cls.sections, cls.symbols, cls.manifest = builder.compile_patch(LLVM_BIN)
        source = bytearray(synthetic_source())
        for offset, before in builder.HOOKS.values():
            source[offset : offset + len(before)] = before
        cls.source = bytes(source)

    def test_reversible_full_and_application_images(self):
        """Full and application sources produce identical bytes and preserve boot/tail."""
        full, spans = builder.apply_sections(
            self.source, 0, self.sections, self.symbols
        )
        app, _ = builder.apply_sections(
            self.source[platform.APP_OFFSET :],
            platform.APP_OFFSET,
            self.sections,
            self.symbols,
        )
        self.assertEqual(full[platform.APP_OFFSET :], app)
        self.assertEqual(
            full[: platform.APP_OFFSET], self.source[: platform.APP_OFFSET]
        )
        self.assertEqual(
            full[platform.PAYLOAD_END :], self.source[platform.PAYLOAD_END :]
        )
        restored = bytearray(full)
        for span in spans:
            offset = span["image_offset"]
            restored[offset : offset + span["size"]] = bytes.fromhex(span["before"])
        self.assertEqual(restored, self.source)
        self.assertEqual(self.sections["descriptor"][:8], b"T150BZ01")

    def test_timer_hook_source_and_size(self):
        """A changed native RTC instruction or oversized replacement is rejected."""
        source = bytearray(self.source)
        source[0x67EC] ^= 1
        with self.assertRaises(platform.ImageError):
            builder.apply_sections(bytes(source), 0, self.sections, self.symbols)
        sections = dict(self.sections)
        sections["hook_buzzer_timer"] += b"\0\0"
        with self.assertRaises(platform.ImageError):
            builder.apply_sections(self.source, 0, sections, self.symbols)

    def test_changed_code_and_section_set_rejected(self):
        """Compiler output cannot omit a hook or change a pinned instruction."""
        for name in ("candidate", "hook_buzzer_timer"):
            for missing in (False, True):
                sections = dict(self.sections)
                sections.pop("descriptor")
                if missing:
                    sections.pop(name)
                else:
                    sections[name] = bytes([sections[name][0] ^ 1]) + sections[name][1:]
                with (
                    patch.object(
                        builder.compiler,
                        "compile_patch",
                        return_value=(sections, self.symbols, self.manifest),
                    ),
                    self.assertRaises(platform.ImageError),
                ):
                    builder.compile_patch(LLVM_BIN)

    def test_unknown_image_and_dirty_acceptance_build_rejected(self):
        """Reject unknown source hashes and dirty acceptance builds before compilation."""
        for source in (b"", self.source, self.source[platform.APP_OFFSET :]):
            with patch.object(builder, "compile_patch") as compile_mock:
                with self.assertRaises(platform.ImageError):
                    builder.build(source, LLVM_BIN)
                compile_mock.assert_not_called()
        with (
            patch.object(platform, "identify", return_value=0),
            patch.object(
                platform, "_source_provenance", side_effect=platform.ImageError("dirty")
            ) as provenance,
            patch.object(builder, "compile_patch") as compile_mock,
        ):
            with self.assertRaises(platform.ImageError):
                builder.build(self.source, LLVM_BIN, require_clean_source=True)
            provenance.assert_called_once_with(True)
            compile_mock.assert_not_called()


def main():
    """Run the new image checks using the pinned toolchain."""
    global LLVM_BIN
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-bin", type=Path)
    LLVM_BIN = parser.parse_args().llvm_bin
    unittest.main(argv=[__file__], verbosity=2)


if __name__ == "__main__":
    main()
