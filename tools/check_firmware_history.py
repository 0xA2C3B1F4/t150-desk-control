#!/usr/bin/env python3
"""Check frozen historical reconstruction headers without rebuilding old firmware."""

import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
HEADERS = (
    "dt02_patch.h",
    "child_lock_patch.h",
    "reset_speed_patch.h",
    "tb01_patch.h",
    "platform_patch.h",
    "motion_trim_patch.h",
)


def fingerprints(data: bytes) -> dict:
    """Hash the complete reconstructor and each literal replacement byte array."""
    arrays = re.findall(
        rb"(?:constexpr|const)\s+uint8_t\s+(\w+)\[\]\s*=\s*\{([^}]+)\}", data
    )
    sections = {}
    for index, (name, body) in enumerate(arrays):
        values = bytes(
            int(value.strip(), 0) for value in body.split(b",") if value.strip()
        )
        sections[f"{index}:{name.decode()}"] = hashlib.sha256(values).hexdigest()
    if not sections:
        raise ValueError("Historical header has no literal patch sections")
    return {"sha256": hashlib.sha256(data).hexdigest(), "sections": sections}


def validate(root: Path = ROOT) -> None:
    """Reject changes to any historical header or its pinned replacement sections."""
    manifest = json.loads((root / "metadata/firmware-history.json").read_text())
    if not re.fullmatch(r"[0-9a-f]{40}", manifest.get("source_commit", "")):
        raise ValueError("Historical source commit is missing or invalid")
    if manifest.get("schema_version") != 1 or set(manifest["headers"]) != set(HEADERS):
        raise ValueError("Historical header manifest differs")
    for name in HEADERS:
        data = (root / "components/desk_firmware_update" / name).read_bytes()
        if fingerprints(data) != manifest["headers"][name]:
            raise ValueError(f"Frozen historical firmware header changed: {name}")


if __name__ == "__main__":
    validate()
    print("Historical firmware headers and replacement sections match")
