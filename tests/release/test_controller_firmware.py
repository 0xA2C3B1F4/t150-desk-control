"""Check current firmware identity and immutable historical reconstruction data."""

import copy
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import build_controller_firmware as current  # noqa: E402 - Load repository tools.
import check_firmware_history as history  # noqa: E402 - Load repository tools.


class ControllerFirmwareTest(unittest.TestCase):
    """Bind the normal builder and release target to the tested controller image."""

    def test_catalog_identity(self):
        """Reject missing or conflicting current-image metadata."""
        catalog = json.loads((ROOT / "metadata/firmware-catalog.json").read_text())
        self.assertEqual(
            current.current_identity(catalog)["sha256"], current.OUTPUT_APP_SHA256
        )
        for mutate in (
            lambda c: c.pop("current"),
            lambda c: c["current"].update(id="fp01"),
            lambda c: c["current"].update(profile="unknown"),
            lambda c: c["prepare"].update(target="fp01"),
            lambda c: c["images"][-1].update(sha256="0" * 64),
            lambda c: c["images"][-1].update(payload_size=32768),
            lambda c: c["images"].append(c["images"][-1]),
        ):
            changed = copy.deepcopy(catalog)
            mutate(changed)
            with self.assertRaises(ValueError):
                current.current_identity(changed)

    def test_cli_rejects_profile_overrides(self):
        """Do not expose analysis-only timing knobs on the normal build command."""
        for option in ("--timing", "--boot-profile", "--write-ticks"):
            result = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/build_controller_firmware.py"),
                    "source.bin",
                    "output",
                    option,
                    "1",
                ],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("unrecognized arguments", result.stderr)

    def test_historical_headers_are_frozen(self):
        """Reject both changed reconstruction code and changed replacement bytes."""
        history.validate(ROOT)
        with tempfile.TemporaryDirectory(dir=os.environ["TMPDIR"]) as temporary:
            root = Path(temporary)
            (root / "metadata").mkdir()
            shutil.copy(ROOT / "metadata/firmware-history.json", root / "metadata")
            headers = root / "components/desk_firmware_update"
            headers.mkdir(parents=True)
            for name in history.HEADERS:
                shutil.copy(ROOT / "components/desk_firmware_update" / name, headers)
            for name in history.HEADERS:
                path = headers / name
                original = path.read_bytes()
                changed_byte = re.sub(
                    rb"0x([0-9a-fA-F]{2})(?=,)",
                    lambda match: f"0x{int(match[1], 16) ^ 1:02x}".encode(),
                    original,
                    count=1,
                )
                self.assertNotEqual(changed_byte, original)
                for changed in (original + b"\n", changed_byte):
                    path.write_bytes(changed)
                    with self.assertRaisesRegex(ValueError, "Frozen historical"):
                        history.validate(root)
                path.write_bytes(original)


if __name__ == "__main__":
    unittest.main()
