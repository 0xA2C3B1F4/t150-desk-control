#!/usr/bin/env python3
"""Check the production image recognizer with real ESPHome SHA-256; no device.

The default test uses synthetic images only. --firmware optionally adds a local
copy of the pinned application, read at runtime and never copied into the build.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from desk_test_support import ROOT, build_host, open_pty, parse_args, test_env

APPLICATION_SIZE = 55296
TRUSTED_SHA256 = "bf52d596c81d6d499480a9dd3173e6991de5ee0c72364e0b53bc91ea89053faf"
# Independent normalization oracle for the pinned application layout.
SCALAR_OFFSETS = (0x17B4, 0x17B8, 0x17BC, 0x5FD4, 0x5FD8)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware", type=Path, help="Optional local copy of the pinned 54 KiB application")
    args = parse_args(parser=parser)
    work = args.work_dir
    env = test_env(work)
    env.pop("T150_PROFILE_TEST_FIRMWARE", None)
    if args.firmware:
        fixture = args.firmware.resolve()
        with fixture.open("rb") as stream:
            image = stream.read(APPLICATION_SIZE + 1)
        if len(image) != APPLICATION_SIZE or hashlib.sha256(image).hexdigest() != TRUSTED_SHA256:
            raise ValueError("--firmware must be the pinned, complete 54 KiB application")
        env["T150_PROFILE_TEST_FIRMWARE"] = str(fixture)

    expected = bytearray((i * 37 + 11) & 255 for i in range(APPLICATION_SIZE))
    for offset in SCALAR_OFFSETS:
        expected[offset:offset + 4] = b"\0" * 4
    normalized_sha = hashlib.sha256(expected).hexdigest()
    cfg = work / "host.yaml"
    cfg.write_text(f"""esphome:
  name: firmware-profile-test
  build_path: {work}/build
  includes:
    - {ROOT}/components/desk_firmware_export/firmware_profile.h
    - {ROOT}/tests/esphome/firmware_profile_regression.h
  on_boot:
    priority: -100
    then:
      - lambda: |-
          firmware_profile_test::run();
          exit(0);
host:
sha256:
external_components:
  - source:
      type: local
      path: {ROOT}/components
    components: [desk_uart]
logger:
  level: DEBUG
uart:
  id: serial
  port: /dev/null
  baud_rate: 115200
desk_uart:
  uart_id: serial
  height:
    name: Height
""")
    master, slave = open_pty()
    try:
        with (work / "build.log").open("w") as log:
            build_host(cfg, work, env, log, uart_port=os.ttyname(slave))
        with (work / "runtime.log").open("w") as log:
            completed = subprocess.run(
                [str(work / "build/.pioenvs/firmware-profile-test/program")],
                cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=30,
            )
        output = (work / "runtime.log").read_text()
        if completed.returncode:
            raise RuntimeError(f"Host checks failed; see {work / 'runtime.log'}")
        match = re.search(r"PROFILE_NORMALIZED_SHA=([0-9a-f]{64})", output)
        assert match and match.group(1) == normalized_sha, "Normalization differs from independent hashlib oracle"
        assert "PROFILE_SYNTHETIC=passed" in output
        assert ("PROFILE_PRIVATE=passed" in output) == bool(args.firmware)
        summary = {
            "passed": True,
            "hardware_access": False,
            "hash_implementation": "Production helper with ESPHome SHA256/OpenSSL",
            "synthetic_normalized_sha256": normalized_sha,
            "private_fixture_sha256": TRUSTED_SHA256 if args.firmware else None,
            "checks": [
                "SHA normalization agrees with independent Python hashlib",
                "Unknown synthetic images and unsupported lengths are rejected",
                "Rejected synthetic images do not grant control approval",
            ],
            "limits": ["Does not execute the ESP-IDF reader, UART transfer or HTTP/image lifetime."],
        }
        if args.firmware:
            summary["checks"] += [
                "Known application and all five scalar variants use the real production recognizer",
                "Non-scalar changes, code shifts, invalid fields and incomplete images are rejected",
                "Recognized images yield research-only profiles; boot coefficients remain unknown",
            ]
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, indent=2))
    finally:
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    main()
