"""Create structurally valid synthetic ESP32-C3 images for release gate tests."""

import hashlib
from pathlib import Path
import struct

import release
import release_binary as binary


def image(payload: bytes) -> bytes:
    """Encode an image with a real segment, XOR checksum and appended hash."""
    header = bytearray(24)
    header[0:4] = bytes([0xE9, 1, 2, 0x20])
    struct.pack_into("<H", header, 12, 5)
    header[23] = 1
    data = header + struct.pack("<II", 0x3C000020, len(payload)) + payload
    checksum = 0xEF
    for value in payload:
        checksum ^= value
    data += b"\0" * ((15 - len(data)) % 16) + bytes([checksum])
    return bytes(data) + hashlib.sha256(data).digest()


def factory(
    version: str,
    commit: str,
    *,
    app_size: int = 0x1C0000,
    payload_size: int = 0,
    esphome: str = "2026.10.1",
    nvs_offset: int = 0x9000,
    nvs_size: int = 0x5000,
    nvs_subtype: int = 2,
) -> bytes:
    """Build a bootloader, checksummed partition table and versioned app image."""
    descriptor = bytearray(256)
    struct.pack_into("<I", descriptor, 0, 0xABCD5432)
    descriptor[16 : 16 + len(esphome)] = esphome.encode()
    descriptor[48:57] = b"t150-desk"
    descriptor[112:118] = b"v5.5.6"
    app = image(
        bytes(descriptor)
        + version.encode()
        + b"\0source:"
        + commit.encode()
        + b"\0"
        + b"\0" * payload_size
    )
    table = bytearray()
    for kind, subtype, offset, size, label in (
        (1, nvs_subtype, nvs_offset, nvs_size, b"nvs"),
        (1, 0, 0xE000, 0x2000, b"otadata"),
        (0, 0x10, 0x10000, app_size, b"app0"),
        (0, 0x11, 0x1D0000, 0x1C0000, b"app1"),
    ):
        table += struct.pack(
            "<HBBII16sI", 0x50AA, kind, subtype, offset, size, label, 0
        )
    table += (
        b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(table, usedforsecurity=False).digest()
    )
    table += b"\xff" * (0xC00 - len(table))
    data = bytearray(b"\xff" * 0x10000)
    boot = image(b"fixture bootloader")
    data[: len(boot)] = boot
    data[0x8000:0x8C00] = table
    return bytes(data) + app


def write_build(root: Path, directory: Path, version: str, commit: str) -> Path:
    """Write only the two explicit build outputs expected by the packager."""
    directory.mkdir(parents=True, exist_ok=True)
    pin = release.esphome_pin(root)
    data = factory(version, commit, esphome=pin)
    report = {
        "schema_version": 1,
        "source_state": "clean",
        "binary": binary.metadata(data, version, commit, pin),
        "image": binary.inspect_factory(data, version, commit),
        "configuration": {
            path: binary.digest((root / path).read_bytes())
            for path in (binary.CONFIG, binary.CONTROLS)
        },
        "sdkconfig_sha256": binary.digest(b"fixture sdkconfig"),
        "toolchain": {
            "esphome": pin,
            "esp_idf": "v5.5.6",
            "compiler": "fixture-compiler",
            "esptool": "fixture-esptool",
        },
    }
    (directory / binary.binary_name(version)).write_bytes(data)
    (directory / binary.REPORT).write_bytes(release.json_bytes(report))
    return directory
