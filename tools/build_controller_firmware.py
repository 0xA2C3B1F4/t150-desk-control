#!/usr/bin/env python3
"""Build the current controller firmware from an exact saved R7 image."""

import argparse
import json
from pathlib import Path
import re
import subprocess

import build_buzzer_patch as recipe
from build_child_lock_patch import ImageError, read_saved_image, sha256

FIRMWARE_ID = "bz01"
FIRMWARE_NAME = "T150 Desk Control controller firmware"
PROFILE = recipe.platform.LAYOUT["profile"]
SOURCE_APP_SHA256 = recipe.platform.SOURCE_APP_SHA256
OUTPUT_APP_SHA256 = recipe.OUTPUT_APP_SHA256
OUTPUT_FULL_SHA256 = recipe.OUTPUT_FULL_SHA256
OUTPUT_PAYLOAD_SHA256 = recipe.OUTPUT_PAYLOAD_SHA256
PAYLOAD_SIZE = recipe.PAYLOAD_SIZE


def current_identity(catalog: dict) -> dict:
    """Resolve the catalog target against the fixed production recipe."""
    if catalog.get("schema_version") != 2 or catalog.get("current") != {
        "id": FIRMWARE_ID,
        "profile": PROFILE,
    }:
        raise ImageError("Catalog current firmware or profile differs")
    targets = [i for i in catalog.get("images", []) if i.get("id") == FIRMWARE_ID]
    if (
        len(targets) != 1
        or targets[0].get("sha256") != OUTPUT_APP_SHA256
        or targets[0].get("payload_size") != PAYLOAD_SIZE
        or catalog.get("prepare", {}).get("target") != FIRMWARE_ID
    ):
        raise ImageError("Catalog target differs from the current firmware recipe")
    return {"id": FIRMWARE_ID, "sha256": OUTPUT_APP_SHA256}


def build(data: bytes, llvm_bin=None, *, require_clean_source=False) -> tuple:
    """Preserve tested controller bytes and add the common build identity."""
    root = recipe.platform.ROOT
    catalog = json.loads((root / "metadata/firmware-catalog.json").read_text())
    identity = current_identity(catalog)
    version_file = root / "components/desk_uart/project_version.json"
    version = json.loads(version_file.read_text())
    if (
        type(version.get("schema_version")) is not int
        or version["schema_version"] != 1
        or not isinstance(version.get("version"), str)
        or not re.fullmatch(
            r"[0-9]{4}\.(?:[1-9]|1[0-2])\.(?:0|[1-9][0-9]*)", version["version"]
        )
    ):
        raise ImageError("Invalid controller target version")
    output, manifest = recipe.build(
        data, llvm_bin, require_clean_source=require_clean_source
    )
    manifest["controller_firmware"] = identity
    manifest["name"] = FIRMWARE_NAME
    manifest["target_version"] = version["version"]
    manifest["source_files"]["build_controller_firmware.py"] = sha256(
        Path(__file__).read_bytes()
    )
    for name in (
        "metadata/firmware-catalog.json",
        "components/desk_uart/project_version.json",
    ):
        manifest["source_files"][name] = sha256((root / name).read_bytes())
    return output, manifest


def main() -> None:
    """Build a new offline bundle with the fixed production profile."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--llvm-bin", type=Path)
    parser.add_argument("--require-clean-source", action="store_true")
    args = parser.parse_args()
    try:
        data = read_saved_image(args.source)
        output, manifest = build(
            data, args.llvm_bin, require_clean_source=args.require_clean_source
        )
        recipe.write_outputs(
            args.output_directory,
            output,
            recipe.platform.identify(data),
            manifest,
            basename="t150-controller",
        )
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
    print(json.dumps(manifest["controller_firmware"]))


if __name__ == "__main__":
    main()
