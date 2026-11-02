#!/usr/bin/env python3
"""Compare informational catalog metadata with compiled admission code."""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

import esphome
import jsonschema

ROOT = Path(__file__).resolve().parents[1]


def production_probe(work):
    """Compile and run the host admission probe in the supplied work directory.

    Create include symlinks and an executable there, then return its JSON output.
    """
    package = Path(esphome.__file__).resolve().parent
    for name in ("desk_uart", "desk_firmware_export", "desk_firmware_update"):
        link = work / "include/esphome/components" / name
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(ROOT / "components" / name, target_is_directory=True)
    flags = ["-std=c++17", "-DUSE_HOST", "-pthread", "-Wall", "-Wextra", "-Werror"]
    for path in [
        ROOT / "tests/esphome/firmware_update_host/include",
        ROOT / "tests/esphome/firmware_export_host/include",
        work / "include",
        ROOT / "components/desk_firmware_update",
        package.parent,
    ]:
        flags.append("-I" + str(path))
    openssl = shlex.split(
        subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "openssl"], text=True
        )
    )
    sources = [
        ROOT / "components/desk_firmware_export/desk_firmware_export.cpp",
        package / "components/sha256/sha256.cpp",
        ROOT / "tests/esphome/firmware_update_host/catalog_probe.cpp",
    ]
    subprocess.run(
        [
            os.environ.get("CXX", "c++"),
            *flags,
            *map(str, sources),
            *openssl,
            "-o",
            str(work / "probe"),
        ],
        check=True,
    )
    return json.loads(subprocess.check_output([str(work / "probe")], text=True))


def validate(catalog, probe):
    """Reject catalog entries that disagree with schema, production or builder pins."""
    schema = json.loads((ROOT / "metadata/firmware-catalog.schema.json").read_text())
    jsonschema.Draft202012Validator(schema).validate(catalog)
    images = catalog["images"]
    if len({x["id"] for x in images}) != len(images):
        raise ValueError("Duplicate firmware ID")
    if {x["id"]: x["sha256"] for x in images} != probe["images"]:
        raise ValueError("Firmware identities differ from production constants")
    for image in images:
        if (
            image["application_size"] != probe["application_size"]
            or image["application_offset"] != 0x2800
            or image["flash_size"] != 65536
        ):
            raise ValueError("Firmware region differs from the supported layout")
    pairs = [tuple(x) for x in catalog["install"]]
    if len(set(pairs)) != len(pairs) or set(pairs) != {
        tuple(x) for x in probe["install"]
    }:
        raise ValueError("Install pairs differ from production admission")
    # Exact builder source/output pins, not catalog-to-schema self-consistency.
    import build_child_lock_patch as cl
    import build_reset_speed_patch as rs

    if (
        cl.SOURCE_APP_SHA256 != probe["images"]["dt02"]
        or cl.OUTPUT_APP_SHA256 != probe["images"]["cl01"]
        or rs.SOURCE_APP_SHA256 != cl.OUTPUT_APP_SHA256
        or rs.OUTPUT_APP_SHA256 != probe["images"]["rs01-r7"]
    ):
        raise ValueError("Patch builder chain differs")
    if catalog["prepare"] != probe["prepare"]:
        raise ValueError("Preparation sources differ from supported cumulative patches")
    if catalog["recovery"] != {
        "target": "stock",
        "documentation": "docs/firmware-update.md",
    }:
        raise ValueError("Recovery is restricted to the guarded original-image path")


def main():
    """Compile the production probe, validate the catalog and print the report.

    Also write the report to --output when requested.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--catalog", type=Path, default=ROOT / "metadata/firmware-catalog.json"
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(
        dir=os.environ["TMPDIR"], prefix="desk-catalog-"
    ) as temp:
        probe = production_probe(Path(temp))
        validate(json.loads(args.catalog.read_text()), probe)
    result = {"passed": True, "production": probe}
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
