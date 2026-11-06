"""Exercise image boundaries and explicit release build provenance offline."""

import copy
import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

import test_release as fixtures

binary_fixture = fixtures.binary_fixture
release = fixtures.release
binary = fixtures.release_binary


class BinaryReleaseTest(unittest.TestCase):
    """Reject malformed image bytes and build evidence before packaging."""

    setUp = fixtures.ReleaseTest.setUp
    git = fixtures.ReleaseTest.git
    binary_build = fixtures.ReleaseTest.binary_build
    package = fixtures.ReleaseTest.package

    def test_actual_image_structures_and_flash_boundaries(self):
        """Check ESP headers, segment lengths, checksums, partitions and target."""
        original = binary_fixture.factory(self.version, self.sha)
        result = binary.inspect_factory(original, self.version, self.sha)
        self.assertLessEqual(
            result["application"]["size"], result["application_partition"]["size"]
        )
        malformed = [
            original[:0x10000],
            original + b"\x00",
            original + b"\xff" * binary.FLASH_SIZE,
            binary_fixture.factory(
                self.version, self.sha, app_size=0x1000, payload_size=0x2000
            ),
            binary_fixture.factory(self.version, self.sha, app_size=0x400000),
        ]
        for offset, value in (
            (0, 0),
            (12, 0),
            (3, 0x30),
            (0x8000, 0),
            (0x8080 + 16, 0),
            (0x10000 + 12, 0),
            (0x10000 + 28, 0xFF),
            (len(original) - 1, 0),
        ):
            changed = bytearray(original)
            changed[offset] = value if changed[offset] != value else value ^ 1
            malformed.append(bytes(changed))
        for index, data in enumerate(malformed):
            with self.subTest(case=index), self.assertRaises(ValueError):
                binary.inspect_factory(data, self.version, self.sha)
        for version, commit in (("2026.11.99", self.sha), (self.version, "f" * 40)):
            with (
                self.subTest(version=version, commit=commit),
                self.assertRaises(ValueError),
            ):
                binary.inspect_factory(original, version, commit)

    def test_factory_requires_complete_erased_nvs_without_prefilled_credentials(self):
        """Reject omitted, truncated and prefilled NVS before accepting USB images."""
        raw = binary_fixture.factory(
            self.version, self.sha, nvs_offset=0x390000, nvs_size=0x70000
        )
        with self.assertRaisesRegex(ValueError, "NVS partition is not fully present"):
            binary.inspect_factory(raw, self.version, self.sha)
        complete = binary.complete_factory(raw)
        self.assertEqual(len(complete), binary.FLASH_SIZE)
        self.assertEqual(complete[: len(raw)], raw)
        self.assertEqual(complete[len(raw) :], b"\xff" * (binary.FLASH_SIZE - len(raw)))
        self.assertTrue(
            binary.inspect_factory(complete, self.version, self.sha)["nvs_erased"]
        )
        with self.assertRaisesRegex(ValueError, "NVS partition is not fully present"):
            binary.inspect_factory(complete[:-1], self.version, self.sha)
        for data, offset in (
            (complete, 0x390000),
            (complete, binary.FLASH_SIZE - 1),
            (binary_fixture.factory(self.version, self.sha), 0x9000),
        ):
            changed = bytearray(data)
            changed[offset] = 0
            with (
                self.subTest(offset=offset),
                self.assertRaisesRegex(
                    ValueError, "NVS partition contains prefilled data"
                ),
            ):
                binary.inspect_factory(bytes(changed), self.version, self.sha)
        missing = binary_fixture.factory(self.version, self.sha, nvs_subtype=3)
        with self.assertRaisesRegex(ValueError, "lacks an NVS partition"):
            binary.inspect_factory(missing, self.version, self.sha)
        with self.assertRaisesRegex(ValueError, "exceeds 4 MiB"):
            binary.complete_factory(complete + b"\xff")

    def test_build_report_binds_image_target_source_and_configuration(self):
        """Reject altered target, source, toolchain and configuration evidence."""
        directory = self.binary_build()
        data, report = binary.read_build(
            directory, self.version, self.sha, release.esphome_pin(self.root)
        )
        for section, key, value in (
            ("binary", "target", "another-board"),
            ("binary", "chip", "ESP32"),
            ("binary", "flash_size", binary.FLASH_SIZE * 2),
            ("binary", "source_sha", "f" * 40),
            ("binary", "sha256", "0" * 64),
            ("binary", "size", len(data) + 1),
            ("binary", "esphome", "other-version"),
            ("toolchain", "esp_idf", "v0"),
            ("toolchain", "compiler", ""),
            ("configuration", binary.CONFIG, "0" * 64),
        ):
            with self.subTest(section=section, key=key):
                changed = copy.deepcopy(report)
                changed[section][key] = value
                (directory / binary.REPORT).write_bytes(release.json_bytes(changed))
                with self.assertRaises(ValueError):
                    release.package_bytes(self.root, self.version, self.sha, directory)
        (directory / binary.REPORT).write_bytes(release.json_bytes(report))
        (directory / "firmware.ota.bin").write_bytes(data)
        with self.assertRaises(ValueError):
            release.package_bytes(self.root, self.version, self.sha, directory)

    def test_named_binary_and_report_are_in_user_checksums(self):
        """Include the direct binary and report in both asset digest lists."""
        package = self.package()
        manifest = release.verify_package(package, self.version, self.sha)
        name = binary.binary_name(self.version)
        self.assertEqual(manifest["schema_version"], 2)
        self.assertEqual(
            manifest["binary"]["sha256"], release.digest((package / name).read_bytes())
        )
        for name in (name, binary.REPORT):
            self.assertIn(name, manifest["assets"])
            self.assertIn("  " + name + "\n", (package / "SHA256SUMS").read_text())
        self.assertEqual([p.suffix for p in package.iterdir()].count(".bin"), 1)

    def test_clean_build_compiles_once_outside_source_and_verifies_own_identity(self):
        """Keep source identity clean while generating only one explicit build."""
        work = Path(self.temp.name) / "work"
        output = Path(self.temp.name) / "output"
        calls = []
        original_run = subprocess.run

        def compile_fixture(command, **kwargs):
            if "esphome" not in command:
                return original_run(command, **kwargs)
            calls.append((command, kwargs))
            native = work / "build/build"
            native.mkdir(parents=True)
            (native / "firmware.factory.bin").write_bytes(
                binary_fixture.factory(self.version, self.sha)
            )
            (native / "project_description.json").write_text(
                json.dumps({"c_compiler": sys.executable})
            )
            (native / "CMakeCache.txt").write_text(
                "PYTHON:UNINITIALIZED=" + sys.executable + "\n"
            )
            (work / "build/sdkconfig.t150-stamp-c3").write_text("fixture")
            defines = work / "build/src/esphome/core/defines.h"
            defines.parent.mkdir(parents=True)
            defines.write_text(
                "".join(
                    f'#define {key} "{value}"\n'
                    for key, value in (
                        ("DESK_SOURCE_REVISION", self.sha),
                        ("DESK_SOURCE_VERSION", self.version),
                        ("DESK_SOURCE_STATE", "clean"),
                    )
                )
            )
            return subprocess.CompletedProcess(command, 0)

        with patch.object(binary.subprocess, "run", side_effect=compile_fixture):
            binary.build(self.root, self.version, self.sha, output, work)
        self.assertEqual(len(calls), 1)
        command, options = calls[0]
        self.assertEqual(command.count("compile"), 1)
        self.assertIn(str(self.root / binary.CONFIG), command)
        self.assertEqual(options["env"]["ESPHOME_DATA_DIR"], str(work / "data"))
        self.assertEqual(release.text(self.root, "status", "--porcelain"), "")
        binary.read_build(
            output, self.version, self.sha, release.esphome_pin(self.root)
        )

    def test_build_refuses_dirty_source_and_existing_or_inside_build_paths(self):
        """Reject nonrelease source and paths that could contaminate its identity."""
        for work in (self.root / "build", self.root):
            with self.subTest(work=work), self.assertRaises(ValueError):
                binary.build(
                    self.root,
                    self.version,
                    self.sha,
                    Path(self.temp.name) / "output",
                    work,
                )
        (self.root / "untracked").write_text("dirty")
        with self.assertRaises(ValueError):
            binary.build(
                self.root,
                self.version,
                self.sha,
                Path(self.temp.name) / "output",
                Path(self.temp.name) / "work",
            )

    def test_native_esptool_version_uses_the_build_python(self):
        """Read merge-tool metadata from IDF's interpreter rather than the caller."""
        native = Path(self.temp.name) / "native"
        native.mkdir()
        executable = native / "penv/bin/python"
        (native / "CMakeCache.txt").write_text(
            "PYTHON:UNINITIALIZED=" + str(executable) + "\n"
        )
        with patch.object(
            binary.subprocess, "check_output", return_value="4.13.0\n"
        ) as query:
            self.assertEqual(
                binary.native_esptool_version(
                    native, {"PYTHONPATH": "foreign", "PATH": "fixture"}
                ),
                "4.13.0",
            )
        command = query.call_args.args[0]
        self.assertEqual(command[0], str(executable))
        self.assertNotIn("PYTHONPATH", query.call_args.kwargs["env"])
        self.assertIn("esptool", command[-1])


if __name__ == "__main__":
    unittest.main()
