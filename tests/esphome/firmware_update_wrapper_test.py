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

    def test_extended_images_must_be_supplied_together(self):
        """Exercise both one-sided CLI combinations through the real parser."""
        with tempfile.TemporaryDirectory(
            dir=os.environ["TMPDIR"], prefix="desk-wrapper-"
        ) as directory:
            for option in ("--timer-buzzer", "--platform"):
                with self.subTest(option=option):
                    work = Path(directory) / option.removeprefix("--")
                    result = subprocess.run(
                        [
                            sys.executable,
                            str(ROOT / "tests/esphome/firmware_update_lifecycle.py"),
                            "--work-dir",
                            str(work),
                            option,
                            "unused-private-image.bin",
                        ],
                        text=True,
                        capture_output=True,
                        timeout=10,
                    )
                    self.assertEqual(
                        result.returncode, 2, result.stdout + result.stderr
                    )
                    self.assertIn(
                        "--timer-buzzer and --platform must be provided together",
                        result.stderr,
                    )
                    self.assertFalse((work / "include").exists())
                    self.assertFalse((work / "update-test").exists())


if __name__ == "__main__":
    unittest.main()
