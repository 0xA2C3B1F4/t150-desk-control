#!/usr/bin/env python3
"""Build and inspect the single Stamp-C3 USB distribution image offline."""

import argparse
import hashlib
from importlib.metadata import version as package_version
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

FLASH_SIZE = 4 * 1024 * 1024
CONFIG = "distribution/stamp-c3.yaml"
CONTROLS = "examples/esphome/packages/stamp-c3-controls.yaml"
REPORT = "binary-build-report.json"
TARGET = "stamp-c3/C056-B"
CHIP = "ESP32-C3"


def digest(data: bytes) -> str:
    """Return a SHA-256 identity for build evidence."""
    return hashlib.sha256(data).hexdigest()


def binary_name(version: str) -> str:
    """Return the only allowed binary attachment name for a release version."""
    if not re.fullmatch(r"[0-9]{4}\.(?:[1-9]|1[0-2])\.(?:0|[1-9][0-9]*)", version):
        raise ValueError("Invalid binary version")
    return f"t150-stamp-c3-{version}.factory.bin"


def complete_factory(data: bytes) -> bytes:
    """Include erased flash through 4 MiB so USB flashing also replaces NVS."""
    if len(data) > FLASH_SIZE:
        raise ValueError("Factory image exceeds 4 MiB")
    return data.ljust(FLASH_SIZE, b"\xff")


def _image(data: bytes, offset: int, limit: int) -> dict:
    """Parse one Espressif image and verify its checksum and appended SHA-256."""
    if offset + 24 > min(len(data), limit):
        raise ValueError("Truncated ESP image header")
    header = data[offset : offset + 24]
    if (
        header[0] != 0xE9
        or not 1 <= header[1] <= 16
        or header[3] >> 4 != 2
        or struct.unpack_from("<H", header, 12)[0] != 5
        or header[23] != 1
    ):
        raise ValueError("Expected a hashed ESP32-C3 image with 4 MiB flash")
    cursor, checksum, first_segment = offset + 24, 0xEF, None
    for _ in range(header[1]):
        if cursor + 8 > min(len(data), limit):
            raise ValueError("Truncated ESP segment header")
        _, size = struct.unpack_from("<II", data, cursor)
        cursor += 8
        if not size or cursor + size > min(len(data), limit):
            raise ValueError("ESP segment exceeds image or app partition")
        if first_segment is None:
            first_segment = cursor
        for value in data[cursor : cursor + size]:
            checksum ^= value
        cursor += size
    checksum_offset = (cursor + 16) // 16 * 16 - 1
    end = checksum_offset + 33
    if end > min(len(data), limit):
        raise ValueError("ESP checksum/hash exceeds image or app partition")
    if any(data[cursor:checksum_offset]) or data[checksum_offset] != checksum:
        raise ValueError("ESP image checksum or alignment padding differs")
    if (
        hashlib.sha256(data[offset : checksum_offset + 1]).digest()
        != data[checksum_offset + 1 : end]
    ):
        raise ValueError("ESP image appended SHA-256 differs")
    return {
        "offset": offset,
        "end": end,
        "size": end - offset,
        "segments": header[1],
        "first_segment": first_segment,
    }


def inspect_factory(data: bytes, version: str, commit: str | None = None) -> dict:
    """Verify factory, partition and app boundaries from the actual image bytes."""
    if not 0x10000 < len(data) <= FLASH_SIZE:
        raise ValueError("Factory image exceeds 4 MiB or lacks an application")
    bootloader = _image(data, 0, 0x8000)
    table = data[0x8000:0x8C00]
    partitions = []
    md5_seen = False
    for index in range(0, len(table), 32):
        entry = table[index : index + 32]
        if entry[:2] == b"\xeb\xeb":
            if (
                entry[2:16] != b"\xff" * 14
                or hashlib.md5(table[:index], usedforsecurity=False).digest()
                != entry[16:]
            ):
                raise ValueError("Partition table MD5 differs")
            if table[index + 32 :] != b"\xff" * (len(table) - index - 32):
                raise ValueError("Unexpected bytes after partition table MD5")
            md5_seen = True
            break
        if entry[:2] != b"\xaa\x50":
            raise ValueError("Invalid or missing partition table entry")
        _, kind, subtype, offset, size, label, flags = struct.unpack(
            "<HBBII16sI", entry
        )
        label = label.rstrip(b"\0").decode("ascii")
        if (
            not label
            or not size
            or flags
            or offset < 0x9000
            or offset % 0x1000
            or offset + size > FLASH_SIZE
            or any(
                label == p["label"]
                or offset < p["offset"] + p["size"]
                and p["offset"] < offset + size
                for p in partitions
            )
        ):
            raise ValueError("Invalid, overlapping or out-of-flash partition")
        partitions.append(
            dict(type=kind, subtype=subtype, offset=offset, size=size, label=label)
        )
    if not md5_seen:
        raise ValueError("Partition table checksum missing")
    nvs_partitions = [p for p in partitions if p["type"] == 1 and p["subtype"] == 2]
    if not nvs_partitions:
        raise ValueError("Factory image lacks an NVS partition")
    for nvs in nvs_partitions:
        end = nvs["offset"] + nvs["size"]
        if end > len(data):
            raise ValueError("NVS partition is not fully present in factory image")
        if data[nvs["offset"] : end] != b"\xff" * nvs["size"]:
            raise ValueError("NVS partition contains prefilled data")
    applications = [p for p in partitions if p["type"] == 0]
    populated = [
        p for p in applications if data[p["offset"] : p["offset"] + 1] == b"\xe9"
    ]
    if len(populated) != 1 or populated[0]["offset"] != 0x10000:
        raise ValueError("Expected exactly one application at 0x10000")
    partition = populated[0]
    app = _image(data, partition["offset"], partition["offset"] + partition["size"])
    if data[app["end"] :] != b"\xff" * (len(data) - app["end"]):
        raise ValueError("Unexpected trailing bytes after factory application")
    descriptor = app["first_segment"]
    if data[descriptor : descriptor + 4] != b"\x32\x54\xcd\xab":
        raise ValueError("ESP-IDF application descriptor missing")
    descriptor_data = {
        name: data[descriptor + start : descriptor + end].split(b"\0", 1)[0].decode()
        for name, start, end in (
            ("version", 16, 48),
            ("project_name", 48, 80),
            ("esp_idf", 112, 144),
        )
    }
    if version.encode() + b"\0" not in data[app["offset"] : app["end"]]:
        raise ValueError("Application project version missing")
    if commit is not None and (
        not re.fullmatch(r"[0-9a-f]{40}", commit)
        or commit.encode() not in data[app["offset"] : app["end"]]
    ):
        raise ValueError("Application lacks its full source SHA")
    return {
        "bootloader": {k: v for k, v in bootloader.items() if k != "first_segment"},
        "partition_table_sha256": digest(table),
        "partitions": partitions,
        "application": {k: v for k, v in app.items() if k != "first_segment"},
        "application_partition": partition,
        "app_descriptor": descriptor_data,
        "nvs_erased": True,
    }


def metadata(data: bytes, version: str, commit: str, esphome: str) -> dict:
    """Describe the named binary and bind it to its source and target."""
    return {
        "name": binary_name(version),
        "sha256": digest(data),
        "size": len(data),
        "target": TARGET,
        "chip": CHIP,
        "flash_size": FLASH_SIZE,
        "source_sha": commit,
        "esphome": esphome,
    }


def verify_build(
    data: bytes, report: dict, version: str, commit: str, esphome: str
) -> dict:
    """Reject substituted image bytes, build identities or incomplete evidence."""
    if (
        report.get("schema_version") != 1
        or report.get("binary") != metadata(data, version, commit, esphome)
        or report.get("source_state") != "clean"
        or report.get("image") != inspect_factory(data, version, commit)
        or set(report.get("configuration", {})) != {CONFIG, CONTROLS}
        or any(
            not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value)
            for value in report["configuration"].values()
        )
        or set(report.get("toolchain", {}))
        != {"esphome", "esp_idf", "compiler", "esptool"}
        or report["toolchain"]["esphome"] != esphome
        or report["image"]["app_descriptor"]["version"] != esphome
        or report["toolchain"]["esp_idf"]
        != report["image"]["app_descriptor"]["esp_idf"]
        or any(
            not isinstance(value, str) or not value
            for value in report["toolchain"].values()
        )
        or not re.fullmatch(r"[0-9a-f]{64}", report.get("sdkconfig_sha256", ""))
    ):
        raise ValueError("Binary build report differs or is incomplete")
    return report["binary"]


def read_build(
    directory: Path, version: str, commit: str, esphome: str, *, exact: bool = True
) -> tuple[bytes, dict]:
    """Read explicit regular build inputs and verify their full evidence."""
    directory = Path(directory)
    names = {binary_name(version), REPORT}
    if exact and {p.name for p in directory.iterdir()} != names:
        raise ValueError("Unexpected binary build output")
    for name in names:
        path = directory / name
        if path.is_symlink() or not path.is_file():
            raise ValueError("Binary build inputs must be regular files")
        if path.stat().st_size > (FLASH_SIZE if name.endswith(".bin") else 1_000_000):
            raise ValueError("Binary build input exceeds allowed size")
    data = (directory / binary_name(version)).read_bytes()
    report = json.loads((directory / REPORT).read_text())
    verify_build(data, report, version, commit, esphome)
    return data, report


def native_esptool_version(native: Path, env: dict[str, str]) -> str:
    """Query esptool through the Python interpreter selected by this IDF build."""
    cache = (native / "CMakeCache.txt").read_text()
    match = re.search(r"^PYTHON:(?:FILEPATH|UNINITIALIZED|STRING)=(.+)$", cache, re.M)
    if not match:
        raise ValueError("Native build Python identity missing from CMake cache")
    native_env = dict(env)
    native_env.pop("PYTHONPATH", None)
    return subprocess.check_output(
        [
            match.group(1),
            "-c",
            "from importlib.metadata import version; print(version('esptool'))",
        ],
        env=native_env,
        text=True,
        timeout=30,
    ).strip()


def build(root: Path, version: str, commit: str, output: Path, work: Path) -> None:
    """Compile once from a clean source with all generated data outside that source."""
    import release

    root = release.validate(root, version, commit)
    work, output = work.resolve(), output.resolve()
    if root == work or root in work.parents or root == output or root in output.parents:
        raise ValueError("Binary build and output must be outside source")
    if work.exists() or output.exists():
        raise ValueError("Binary build and output require new directories")
    pin = release.esphome_pin(root)
    if package_version("esphome") != pin:
        raise ValueError("Installed ESPHome differs from source pin")
    work.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    for key in (
        "IDF_PATH",
        "IDF_TARGET",
        "IDF_PYTHON_ENV_PATH",
        "PYTHONPATH",
        "PLATFORMIO_BUILD_DIR",
    ):
        env.pop(key, None)
    env.update(
        ESPHOME_DATA_DIR=str(work / "data"),
        ESPHOME_ESP_IDF_PREFIX=str(work / "native-idf"),
        IDF_TOOLS_PATH=str(work / "idf-tools"),
        PLATFORMIO_CORE_DIR=str(work / "platformio"),
        PLATFORMIO_BUILD_CACHE_DIR=str(work / "pio-cache"),
        PIP_CACHE_DIR=str(work / "pip-cache"),
        UV_CACHE_DIR=str(work / "uv-cache"),
        CCACHE_DIR=str(work / "ccache"),
        PYTHONDONTWRITEBYTECODE="1",
    )
    with (work / "compile.log").open("wb") as log:
        subprocess.run(
            [
                sys.executable,
                "-m",
                "esphome",
                "-s",
                "build_directory",
                str(work / "build"),
                "compile",
                str(root / CONFIG),
            ],
            env=env,
            cwd=root,
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
        )
    release.validate(root, version, commit)
    defines = (work / "build/src/esphome/core/defines.h").read_text()
    for key, expected in (
        ("DESK_SOURCE_REVISION", commit),
        ("DESK_SOURCE_VERSION", version),
        ("DESK_SOURCE_STATE", "clean"),
    ):
        if f'#define {key} "{expected}"' not in defines:
            raise ValueError("Generated binary source identity differs: " + key)
    native = work / "build/build"
    data = complete_factory((native / "firmware.factory.bin").read_bytes())
    description = json.loads((native / "project_description.json").read_text())
    compiler = subprocess.check_output(
        [description["c_compiler"], "--version"], text=True
    ).splitlines()[0]
    report = {
        "schema_version": 1,
        "source_state": "clean",
        "binary": metadata(data, version, commit, pin),
        "image": inspect_factory(data, version, commit),
        "configuration": {
            path: digest((root / path).read_bytes()) for path in (CONFIG, CONTROLS)
        },
        "sdkconfig_sha256": digest(
            (work / "build/sdkconfig.t150-stamp-c3").read_bytes()
        ),
        "toolchain": {
            "esphome": pin,
            "esp_idf": inspect_factory(data, version, commit)["app_descriptor"][
                "esp_idf"
            ],
            "compiler": compiler,
            "esptool": native_esptool_version(native, env),
        },
    }
    verify_build(data, report, version, commit, pin)
    output.mkdir(parents=True)
    (output / binary_name(version)).write_bytes(data)
    (output / REPORT).write_bytes(release.json_bytes(report))


def main() -> None:
    """Build an explicit clean release candidate without flashing or publication."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["build"])
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    args = parser.parse_args()
    build(args.source_root, args.version, args.commit, args.output, args.build_root)


if __name__ == "__main__":
    main()
