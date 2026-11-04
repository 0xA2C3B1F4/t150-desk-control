#!/usr/bin/env python3
"""Check fixed-profile TR01 construction without manufacturer firmware or hardware."""

import argparse
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import assemble_motion_trim as assembly
import build_motion_trim_patch as builder
import build_platform_patch as platform

LLVM_BIN = None


def synthetic_source():
    """Place documented anchors and hook instructions in otherwise erased flash."""
    source = bytearray(b"\xff" * platform.FULL_SIZE)
    for offset, data in {**platform.SOURCE_ANCHORS, **platform.BOOT_ANCHORS}.items():
        source[offset : offset + len(data)] = data
    for offset, data in platform.HOOKS.values():
        source[offset : offset + len(data)] = data
    for name, (start, end) in assembly.TRIM_HOOKS.items():
        source[start:end] = bytes.fromhex(assembly.HOOK_ORIGINALS[name])
    return bytes(source)


class BuilderTests(unittest.TestCase):
    """Exercise pins, source admission and atomic output using actual project sections."""

    @classmethod
    def setUpClass(cls):
        """Compile the pinned sections once and derive a synthetic output fixture."""
        cls.sections, cls.symbols, cls.compilation = builder.compile_patch(LLVM_BIN)
        cls.source = synthetic_source()
        cls.output, cls.spans = assembly._apply_sections(
            cls.source,
            0,
            {k: v for k, v in cls.sections.items() if k != "descriptor"},
            cls.symbols,
        )

    def test_fixed_sections(self):
        """Bind generated code to the tested profile and exact section identity."""
        self.assertEqual(
            builder.sections_sha256(self.sections), builder.PATCH_SECTIONS_SHA256
        )
        self.assertEqual(self.compilation["boot_profile"], builder.BOOT_PROFILE)
        self.assertEqual(self.sections["descriptor"][:8], b"T150TR01")

    def test_missing_and_extra_sections_rejected(self):
        """Omitted and unexpected sections cannot produce an accepted digest."""
        for extra in (False, True):
            sections = dict(self.sections)
            if extra:
                sections["unexpected"] = b"\x00"
            else:
                sections.pop("hook_trim_direct")
            with self.assertRaises(platform.ImageError):
                builder.sections_sha256(sections)

    def test_changed_sections_rejected(self):
        """The builder rejects a compiler result containing a changed code byte."""
        sections = {k: v for k, v in self.sections.items() if k != "descriptor"}
        sections["candidate"] = (
            bytes([sections["candidate"][0] ^ 1]) + sections["candidate"][1:]
        )
        with (
            patch.object(
                builder.compiler,
                "compile_patch",
                return_value=(sections, self.symbols, self.compilation),
            ),
            self.assertRaises(platform.ImageError),
        ):
            builder.compile_patch(LLVM_BIN)

    def test_missing_pins_rejected_before_compile(self):
        """All output and section pins must remain present and well formed."""
        for name in (
            "OUTPUT_FULL_SHA256",
            "OUTPUT_APP_SHA256",
            "OUTPUT_PAYLOAD_SHA256",
            "PATCH_SECTIONS_SHA256",
        ):
            with (
                self.subTest(name=name),
                patch.object(builder, name, ""),
                patch.object(builder.compiler, "compile_patch") as compile_mock,
            ):
                with self.assertRaises(platform.ImageError):
                    builder.compile_patch(LLVM_BIN)
                compile_mock.assert_not_called()

    def test_unknown_input_rejected_before_compile(self):
        """Correct size alone does not admit an unknown image."""
        for data in (b"", self.source, self.source[platform.APP_OFFSET :]):
            with patch.object(builder, "compile_patch") as compile_mock:
                with self.assertRaises(platform.ImageError):
                    builder.build(data, LLVM_BIN)
                compile_mock.assert_not_called()

    def test_clean_source_gate_precedes_compile(self):
        """An acceptance build cannot ignore the clean-checkout requirement."""
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

    def test_full_and_application_reconstruct_identically(self):
        """The same project sections preserve boot and tail for both input formats."""
        app, _ = assembly._apply_sections(
            self.source[platform.APP_OFFSET :],
            platform.APP_OFFSET,
            {k: v for k, v in self.sections.items() if k != "descriptor"},
            self.symbols,
        )
        self.assertEqual(app, self.output[platform.APP_OFFSET :])
        self.assertEqual(
            self.output[: platform.APP_OFFSET], self.source[: platform.APP_OFFSET]
        )
        self.assertEqual(
            self.output[platform.PAYLOAD_END :], self.source[platform.PAYLOAD_END :]
        )

    def test_hook_corruption_rejected(self):
        """Every trim hook is checked before replacing original instructions."""
        sections = {k: v for k, v in self.sections.items() if k != "descriptor"}
        for start, _ in assembly.TRIM_HOOKS.values():
            source = bytearray(self.source)
            source[start] ^= 1
            with self.subTest(offset=start), self.assertRaises(platform.ImageError):
                assembly._apply_sections(bytes(source), 0, sections, self.symbols)

    def test_existing_output_preserved(self):
        """A build never replaces an existing output directory."""
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp) / "existing"
            directory.mkdir()
            marker = directory / "keep"
            marker.write_text("existing")
            with self.assertRaises(FileExistsError):
                builder.write_outputs(directory, self.output, 0, {})
            self.assertEqual(marker.read_text(), "existing")

    def test_partial_output_removed(self):
        """A failed bundle write leaves no partially prepared image directory."""
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp) / "bundle"
            original = Path.open

            def fail_manifest(path, *args, **kwargs):
                if path.name == "manifest.json":
                    raise OSError("injected write failure")
                return original(path, *args, **kwargs)

            with patch.object(Path, "open", fail_manifest), self.assertRaises(OSError):
                builder.write_outputs(directory, self.output, 0, {})
            self.assertFalse(directory.exists())


def main():
    """Run the builder checks with the pinned LLVM toolchain."""
    global LLVM_BIN
    parser = argparse.ArgumentParser()
    parser.add_argument("--llvm-bin", type=Path)
    args = parser.parse_args()
    LLVM_BIN = args.llvm_bin
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(BuilderTests)
    )
    raise SystemExit(not result.wasSuccessful())


if __name__ == "__main__":
    main()
