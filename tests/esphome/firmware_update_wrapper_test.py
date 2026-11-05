"""Check optional lifecycle arguments cannot claim unexecuted extended coverage."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WrapperArgumentsTest(unittest.TestCase):
    """Reject incomplete private-image combinations before compiling a fixture."""

    def test_each_private_image_is_required(self):
        """Reject each missing image before compiling or reading private files."""
        options = (
            "--original",
            "--patched",
            "--child-lock",
            "--reset-speed",
            "--timer-buzzer",
            "--platform",
            "--motion-trims",
            "--current",
            "--buzzer",
            "--display-timeout",
        )
        with tempfile.TemporaryDirectory(
            dir=os.environ["TMPDIR"], prefix="desk-wrapper-"
        ) as directory:
            for missing in options:
                with self.subTest(missing=missing):
                    work = Path(directory) / missing.removeprefix("--")
                    arguments = [
                        item
                        for option in options
                        if option != missing
                        for item in (option, "unused-private-image.bin")
                    ]
                    result = subprocess.run(
                        [
                            sys.executable,
                            str(ROOT / "tests/esphome/firmware_update_lifecycle.py"),
                            "--work-dir",
                            str(work),
                            *arguments,
                        ],
                        text=True,
                        capture_output=True,
                        timeout=10,
                    )
                    self.assertEqual(
                        result.returncode, 2, result.stdout + result.stderr
                    )
                    self.assertIn(
                        "requires all ten images; missing " + missing + "\n",
                        result.stderr,
                    )
                    self.assertFalse((work / "include").exists())
                    self.assertFalse((work / "update-test").exists())


if __name__ == "__main__":
    unittest.main()
