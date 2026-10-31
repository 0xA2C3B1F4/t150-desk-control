#!/usr/bin/env python3
"""No-vendor, no-device CL01 builder and protocol unit checks.

Run: PYTHONDONTWRITEBYTECODE=1 python3 tools/test_child_lock_builder.py
No Unicorn, LLVM, saved firmware, or third-party Python packages are required.
Placement tests use a synthetic source with locally substituted identity hashes
and a stub compiler. Real compiled-byte execution is a separate verifier.
"""
from __future__ import annotations

import contextlib
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import build_child_lock_patch as builder
import child_lock_protocol as protocol


def synthetic_source():
    image = bytearray((index * 17 + 13) & 255 for index in range(65536))
    image[0x4388:0x4390] = bytes.fromhex("0246128a00200a42")
    image[0x85F8:0x85FC] = bytes.fromhex("fff712f8")
    image[0x7BAE:0x7BB2] = bytes.fromhex("00f02dfe")
    image[0x8C00:0x9400] = b"\xff" * 2048
    return bytes(image)


class BuilderTests(unittest.TestCase):
    def setUp(self):
        self.source = synthetic_source()
        self.sections = {"gpio_hook": b"G" * 8, "boot_hook": b"B" * 4,
                         "hook": b"H" * 4, "cave": b"D" * 916,
                         "child_lock": b"C" * 794}
        self.symbols = {"child_lock_load": 0x08008C56}
        expected = bytearray(self.source)
        for section, offset in (("gpio_hook", 0x4388), ("boot_hook", 0x85F8),
                                ("hook", 0x7BAE), ("cave", 0x880C), ("child_lock", 0x8C00)):
            value = self.sections[section]
            expected[offset:offset + len(value)] = value
        self.expected_full = bytes(expected)

    def identities(self):
        return patch.multiple(builder, SOURCE_FULL_SHA256=builder.sha256(self.source),
                              SOURCE_APP_SHA256=builder.sha256(self.source[0x2800:]),
                              OUTPUT_FULL_SHA256=builder.sha256(self.expected_full),
                              OUTPUT_APP_SHA256=builder.sha256(self.expected_full[0x2800:]))

    def test_real_hashes_refuse_synthetic_and_mutated_sources_before_compiling(self):
        for value in (self.source, self.source[0x2800:], b"", b"\xff" * 65536,
                      b"\xff" * 55296, b"\xff" * 65535):
            with self.subTest(size=len(value)), patch.object(builder, "compile_patch") as compile_mock:
                with self.assertRaises(builder.ImageError):
                    builder.build(value)
                compile_mock.assert_not_called()
        mutated = bytearray(self.source)
        mutated[0x3000] ^= 1
        with self.identities(), patch.object(builder, "compile_patch") as compile_mock:
            with self.assertRaises(builder.ImageError):
                builder.build(bytes(mutated))
            compile_mock.assert_not_called()

    def test_full_and_application_outputs_match_and_preserve_other_bytes(self):
        with self.identities(), patch.object(builder, "compile_patch", return_value=(self.sections, self.symbols)):
            full, full_manifest = builder.build(self.source)
            app, app_manifest = builder.build(self.source[0x2800:])
        self.assertEqual(full[0x2800:], app)
        self.assertEqual(full[:0x2800], self.source[:0x2800])
        self.assertEqual(full[0x9400:], self.source[0x9400:])
        self.assertEqual(full[0x3E56:0x3E5A], self.source[0x3E56:0x3E5A])
        allowed = set()
        locations = {"gpio_hook": 0x4388, "boot_hook": 0x85F8,
                     "hook": 0x7BAE, "cave": 0x880C, "child_lock": 0x8C00}
        for name, offset in locations.items():
            replacement = self.sections[name]
            self.assertEqual(full[offset:offset + len(replacement)], replacement)
            allowed.update(range(offset, offset + len(replacement)))
        self.assertTrue(all(a == b or index in allowed
                            for index, (a, b) in enumerate(zip(self.source, full))))
        self.assertEqual(len(full), 65536)
        self.assertEqual(len(app), 55296)
        self.assertEqual(full_manifest["output_sha256"], builder.sha256(full))
        self.assertEqual(full_manifest["output_application_sha256"], builder.sha256(app))
        self.assertEqual(app_manifest["output_sha256"], builder.sha256(app))
        self.assertEqual(full_manifest["symbols"], self.symbols)
        self.assertFalse(full_manifest["hardware_tested"])
        self.assertFalse(full_manifest["installable_approved"])
        self.assertEqual(app_manifest["image_origin"], 0x2800)
        for full_span, app_span in zip(full_manifest["patch_spans"], app_manifest["patch_spans"]):
            self.assertEqual(full_span["address"], app_span["address"])
            self.assertEqual(full_span["image_offset"] - 0x2800, app_span["image_offset"])

    def test_local_hook_and_erased_cave_checks_reject_inconsistent_sources(self):
        for offset in (0x4388, 0x85F8, 0x7BAE, 0x8C00, 0x93FF):
            source = bytearray(self.source)
            source[offset] ^= 1
            with self.subTest(offset=hex(offset)), patch.object(builder, "identify", return_value=0), \
                    patch.object(builder, "compile_patch", return_value=(self.sections, self.symbols)):
                with self.assertRaises(builder.ImageError):
                    builder.build(bytes(source))

    def test_unexpected_compiler_output_is_refused_by_final_hash(self):
        modified = dict(self.sections)
        modified["child_lock"] = b"X" + modified["child_lock"][1:]
        with self.identities(), patch.object(builder, "compile_patch", return_value=(modified, self.symbols)):
            for source in (self.source, self.source[0x2800:]):
                with self.assertRaises(builder.ImageError):
                    builder.build(source)

    def test_cli_refuses_existing_output_without_building(self):
        temporary = os.environ.get("TMPDIR")
        self.assertTrue(temporary and Path(temporary).is_dir(), "TMPDIR must be configured")
        with tempfile.TemporaryDirectory(prefix="cl01-unit-", dir=temporary) as directory:
            output = Path(directory) / "existing.bin"
            output.write_bytes(b"preserve")
            argv = ["build_child_lock_patch.py", "unused-source", str(output),
                    "--manifest", str(Path(directory) / "manifest.json")]
            with patch.object(sys, "argv", argv), patch.object(builder, "build") as build_mock, \
                    contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    builder.main()
            self.assertEqual(error.exception.code, 2)
            build_mock.assert_not_called()
            self.assertEqual(output.read_bytes(), b"preserve")

    def test_saved_image_reader_accepts_regular_full_and_app_files(self):
        temporary = os.environ.get("TMPDIR")
        self.assertTrue(temporary and Path(temporary).is_dir(), "TMPDIR must be configured")
        with tempfile.TemporaryDirectory(prefix="cl01-unit-read-", dir=temporary) as directory:
            source = Path(directory) / "saved.bin"
            for data in (self.source, self.source[0x2800:]):
                source.write_bytes(data)
                self.assertEqual(builder.read_saved_image(source), data)

    def test_saved_image_reader_refuses_symlinks_fifos_and_wrong_sizes_before_opening(self):
        temporary = os.environ.get("TMPDIR")
        self.assertTrue(temporary and Path(temporary).is_dir(), "TMPDIR must be configured")
        with tempfile.TemporaryDirectory(prefix="cl01-unit-read-", dir=temporary) as directory:
            directory = Path(directory)
            regular = directory / "saved.bin"
            regular.write_bytes(self.source)
            symlink = directory / "source-link.bin"
            symlink.symlink_to(regular)
            fifo = directory / "input-fifo"
            os.mkfifo(fifo)
            wrong_size = directory / "short.bin"
            wrong_size.write_bytes(self.source[:-1])
            for path in (symlink, fifo, wrong_size, directory):
                with self.subTest(path=path.name), patch.object(builder.os, "open") as open_mock:
                    with self.assertRaises(builder.ImageError):
                        builder.read_saved_image(path)
                    open_mock.assert_not_called()


class ProtocolTests(unittest.TestCase):
    @staticmethod
    def body():
        return bytes.fromhex("d4 01 02 00 00 01 00 43 4c 30 31 fa fd fd fe")

    def test_known_request_wires(self):
        self.assertEqual(protocol.request("get", 0), bytes.fromhex("fa d3 01 00 00 00 d2 fd"))
        self.assertEqual(protocol.request("lock", 0xFAFD), bytes.fromhex("fa d3 01 01 fe fa fe fd d4 fd"))
        self.assertEqual(protocol.request("unlock", 0xFEFF), bytes.fromhex("fa d3 01 02 fe fe ff d1 fd"))

    def test_request_rejects_unknown_operations_and_out_of_range_nonces(self):
        for operation, nonce in (("LOCK", 1), ("stop", 1), ("", 1), ("get", -1), ("unlock", 65536)):
            with self.subTest(operation=operation, nonce=nonce), self.assertRaises(ValueError):
                protocol.request(operation, nonce)

    def test_round_trip_all_bytes_and_reserved_escapes(self):
        body = bytes(range(256))
        encoded = protocol.frame(body)
        self.assertEqual(protocol.unframe(encoded), body)
        self.assertIn(bytes.fromhex("fe fa"), encoded)
        self.assertIn(bytes.fromhex("fe fd"), encoded)
        self.assertIn(bytes.fromhex("fe fe"), encoded)

    def test_strict_reply_fields_and_nonce(self):
        result = protocol.reply(protocol.frame(self.body()), expected_nonce=0xFAFD)
        self.assertEqual(result, {"build": "CL01", "operation": "unlock", "status": "ok",
                                  "operation_raw": 2,
                                  "locked": False, "physical_gate": True, "storage_fault": False,
                                  "nonce": 0xFAFD, "sequence": 0xFDFE})
        for offset, value in ((0, 0xD3), (1, 0), (2, 3), (3, 4), (4, 2), (5, 2), (6, 2), (10, 0x32)):
            body = bytearray(self.body())
            body[offset] = value
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                protocol.reply(protocol.frame(bytes(body)))
        for body in (self.body()[:-1], self.body() + b"\x00"):
            with self.assertRaises(ValueError):
                protocol.reply(protocol.frame(body))
        with self.assertRaises(ValueError):
            protocol.reply(protocol.frame(self.body()), expected_nonce=0xFAFE)

    def test_unknown_operation_is_accepted_only_in_bad_request_replies(self):
        for operation in (3, 127, 255):
            for status in (0, 1, 2, 3):
                body = bytearray(self.body())
                body[2], body[3] = operation, status
                with self.subTest(operation=operation, status=status):
                    if status == 1:
                        result = protocol.reply(protocol.frame(bytes(body)), expected_nonce=0xFAFD)
                        self.assertEqual(result["operation"], "unknown")
                        self.assertEqual(result["operation_raw"], operation)
                        self.assertEqual(result["status"], "bad_request")
                    else:
                        with self.assertRaises(ValueError):
                            protocol.reply(protocol.frame(bytes(body)))

    def test_malformed_frames_are_rejected(self):
        valid = protocol.frame(self.body())
        bad_checksum = bytearray(valid)
        bad_checksum[-2] ^= 1
        malformed = [b"", valid[:-1], valid[1:], b"\xfa\xfe\xfd", b"\xfa\xfe\x01\xfd",
                     b"\xfa\xfa\xfd", b"\xfa\xfd\xfd", valid + valid, bytes(bad_checksum)]
        malformed.extend(valid[:end] for end in range(1, len(valid)))
        for wire in malformed:
            with self.subTest(wire=wire.hex()), self.assertRaises(ValueError):
                protocol.reply(wire)


if __name__ == "__main__":
    unittest.main(verbosity=2)
