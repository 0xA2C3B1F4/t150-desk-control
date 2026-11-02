"""Keep the patch assembly comparison strict across LLVM distributions."""

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
# The generator is a checkout tool rather than an installed package.
sys.path.insert(0, str(ROOT / "tools"))
from generate_reset_speed_core import check_assembly  # noqa: E402

ASSEMBLY = (
    "// reset_speed.c SHA-256: source-digest\n"
    '\t.file\t"reset_speed.c"\n'
    ".Lguard:\n"
    "\tadds\tr0, r1, r2\n"
    "\t.word\t0x20000fd0\n"
    '\t.ident\t"Homebrew clang version 23.1.0"\n'
    "\t.addrsig\n"
)
IDENTITY = '\t.ident\t"Homebrew clang version 23.1.0"\n'


class PatchAssemblyTest(unittest.TestCase):
    """Allow compiler metadata differences without hiding source or code changes."""

    def test_compiler_distribution_string_may_differ(self):
        """Accept the upstream banner without changing either input string."""
        upstream = ASSEMBLY.replace("Homebrew clang", "clang")
        check_assembly(ASSEMBLY, ASSEMBLY)
        check_assembly(ASSEMBLY, upstream)
        self.assertIn("Homebrew clang", ASSEMBLY)
        self.assertNotIn("Homebrew", upstream)

    def test_other_assembly_changes_are_rejected(self):
        """Keep instructions, data, labels, source markers and layout exact."""
        mutations = {
            "instruction": ASSEMBLY.replace("adds", "subs"),
            "data": ASSEMBLY.replace("0x20000fd0", "0x20000fd4"),
            "label": ASSEMBLY.replace(".Lguard:", ".Lother:"),
            "source digest": ASSEMBLY.replace("source-digest", "stale-digest"),
            "file": ASSEMBLY.replace('"reset_speed.c"', '"other.c"'),
            "identity location": ASSEMBLY.replace(IDENTITY, "") + IDENTITY,
            "identity indentation": ASSEMBLY.replace("\t.ident", "  .ident"),
        }
        for name, changed in mutations.items():
            with self.subTest(name=name), self.assertRaises(ValueError):
                check_assembly(ASSEMBLY, changed)

    def test_missing_or_repeated_identity_is_rejected(self):
        """Reject changed metadata structure instead of dropping arbitrary lines."""
        for changed in (ASSEMBLY.replace(IDENTITY, ""), ASSEMBLY + IDENTITY):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                check_assembly(ASSEMBLY, changed)

    def test_malformed_identity_is_rejected(self):
        """Reject escaping, extra operands and executable text after the string."""
        for line in (
            '\t.ident\t"clang\\version"\n',
            '\t.ident\t"clang", "extra"\n',
            '\t.ident\t"clang"; .word 42\n',
            "\t.ident\t42\n",
        ):
            with self.subTest(line=line), self.assertRaises(ValueError):
                check_assembly(ASSEMBLY, ASSEMBLY.replace(IDENTITY, line))


if __name__ == "__main__":
    unittest.main()
