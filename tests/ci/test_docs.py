"""Check Markdown validation without network access or repository mutation."""

import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_docs  # noqa: E402 - Repository tools are outside the test import path.


class DocumentationTest(unittest.TestCase):
    """Detect broken file references and fences while preserving valid Markdown."""

    def setUp(self):
        """Create an isolated document tree on the configured scratch volume."""
        self.temp = tempfile.TemporaryDirectory(dir=os.environ["TMPDIR"])
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.doc = self.root / "README.md"

    def check(self, text):
        """Return validation errors for the supplied Markdown."""
        self.doc.write_bytes(text.encode())
        return check_docs.check_document(self.root, self.doc)

    def test_valid_links_and_code(self):
        """Ignore external URLs and code examples, but resolve reference links."""
        (self.root / "with space.md").write_text("# Target\n")
        self.assertEqual(
            self.check(
                "# Example\n\n[File][ref]\n\n[ref]: with%20space.md#target\n\n"
                "![image](with%20space.md)\n[External](https://example.invalid)\n"
                "```text\n[Example only](missing)\n```\n"
            ),
            [],
        )

    def test_missing_files_and_escape(self):
        """Reject missing references, images and paths outside the repository."""
        for link in [
            "[bad](missing.md)",
            "![bad](missing.png)",
            "[bad](../outside.md)",
        ]:
            self.assertTrue(self.check(link + "\n"))

    def test_fences_and_whitespace(self):
        """Reject unclosed fences and stray whitespace, allowing hard breaks."""
        for text in ["```python\nprint(1)\n", "text \n", "text\r\n", "no newline"]:
            self.assertTrue(self.check(text))
        self.assertEqual(self.check("text  \nnext\n\n~~~\ncode\n~~~\n"), [])

    def test_symlink_document(self):
        """A document symlink cannot substitute content outside the checked tree."""
        target = self.root / "target"
        target.write_text("content\n")
        self.doc.symlink_to(target)
        self.assertTrue(check_docs.check_document(self.root, self.doc))


if __name__ == "__main__":
    unittest.main()
