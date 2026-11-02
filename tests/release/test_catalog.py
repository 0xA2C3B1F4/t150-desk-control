"""Check that catalog mutations cannot bypass firmware metadata validation."""

import copy
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_firmware_catalog as checker  # noqa: E402 - Load tools after the sys.path bootstrap.


class CatalogTest(unittest.TestCase):
    """Exercise catalog rejection with a matching in-memory probe fixture."""

    def setUp(self):
        """Load a catalog and matching probe fixture for metadata mutations."""
        self.catalog = json.loads((ROOT / "metadata/firmware-catalog.json").read_text())
        # Mutations test the validator; CI also builds the independent production probe.
        self.probe = {
            "application_size": 55296,
            "images": {x["id"]: x["sha256"] for x in self.catalog["images"]},
            "install": copy.deepcopy(self.catalog["install"]),
            "prepare": copy.deepcopy(self.catalog["prepare"]),
        }

    def test_rejects_metadata_divergence(self):
        """Reject divergent image metadata and unsupported firmware transitions."""
        mutations = [
            lambda c: c["images"][0].update(sha256="0" * 64),
            lambda c: c["images"][0].update(application_size=65536),
            lambda c: c["images"][0].update(id="unknown"),
            lambda c: c["images"].append(c["images"][0]),
            lambda c: c["install"].pop(),
            lambda c: c["install"].append(["unknown", "stock"]),
            lambda c: c["prepare"]["dt02"].append("rs01-r7"),
            lambda c: c["recovery"].update(target="rs01-r7"),
        ]
        checker.validate(self.catalog, self.probe)
        for mutation in mutations:
            c = copy.deepcopy(self.catalog)
            mutation(c)
            with (
                self.subTest(catalog=c),
                self.assertRaises((ValueError, checker.jsonschema.ValidationError)),
            ):
                checker.validate(c, self.probe)


if __name__ == "__main__":
    unittest.main()
