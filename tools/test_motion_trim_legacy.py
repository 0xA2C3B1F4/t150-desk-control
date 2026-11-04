#!/usr/bin/env python3
"""Run unchanged legacy regressions on the complete TR01 analysis image.

The real platform bootstrap, module initialization, trim boot admission and
controller poll run before each legacy machine starts. A typed test-only C
fixture supplies successful Default boot-read completion instead of modeling
EEPROM transport. It does not qualify I2C timing or physical initialization.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import traceback
from unittest.mock import patch

from unicorn import __version__ as unicorn_version
from unicorn.arm_const import (
    UC_ARM_REG_LR,
    UC_ARM_REG_PC,
    UC_ARM_REG_PRIMASK,
    UC_ARM_REG_SP,
    UC_ARM_REG_XPSR,
)

from assemble_motion_trim import assemble
import build_child_lock_patch as child_builder
from build_reset_speed_patch import find_tool
import test_child_lock_patch as child
import test_reset_speed_patch as reset
import test_uart_parser_boundary as parser_boundary

ROOT = Path(__file__).resolve().parents[1]
FIXTURE_ADDRESS = child.FLASH + 0x10000
SYNTHETIC_BOOT_PROFILE = {
    "timing": 0x10420F13,
    "transfer_ticks": 3,
    "write_ticks": 5,
    "poll_ticks": 1,
    "stop_ticks": 1,
    "max_poll_gap": 1,
    "address": 0x50,
    "record_address": 0x62,
    "delivery_ticks": 2,
}
BOOT_COMPLETION = r"""
#include "motion_trim_layout.h"

int legacy_complete_boot(void) {
  TrimAdapterState* adapter = (TrimAdapterState*)PLATFORM_TRIM_ADAPTER_ADDRESS;
  TrimOwnerState* owner = (TrimOwnerState*)PLATFORM_TRIM_OWNER_ADDRESS;
  TrimStorageState* record = (TrimStorageState*)PLATFORM_TRIM_RECORD_ADDRESS;
  TrimI2cState* driver = (TrimI2cState*)PLATFORM_TRIM_I2C_ADDRESS;
  if (adapter->owner != owner || adapter->record != record || adapter->driver != driver ||
      adapter->mode != TRIM_STORAGE_BOOT || adapter->phase != TRIM_ADAPTER_WAIT ||
      owner->phase != TRIM_OWNER_TRIM || owner->outstanding || driver->result == TRIM_I2C_PENDING) {
    return 0;
  }
  /* Model the transport certificate, not controller publication or handoff.
   * Real adapter_poll and controller_poll consume this completed owner state. */
  record->mode = TRIM_STORAGE_TERMINAL;
  record->status = TRIM_DEFAULT;
  for (unsigned i = 0; i < TRIM_COUNT; ++i) {
    record->active[i] = 0;
  }
  driver->result = TRIM_I2C_OK;
  driver->released = 1;
  owner->phase = TRIM_OWNER_TERMINAL;
  owner->result = TRIM_OWNER_OK;
  owner->trim_verified = 1;
  owner->bus_ready = 1;
  owner->handoff = 1;
  return 1;
}
"""


def digest(data: bytes) -> str:
    """Return the SHA-256 binding for an input, source or fixture."""
    return hashlib.sha256(data).hexdigest()


def compile_fixture(llvm_bin: Path | None) -> bytes:
    """Compile a typed transport-completion fixture outside the tested image."""
    clang = find_tool("clang", llvm_bin)
    linker = find_tool("ld.lld", llvm_bin)
    objcopy = find_tool("llvm-objcopy", llvm_bin)
    with tempfile.TemporaryDirectory(
        prefix="trim-legacy-", dir=os.environ["TMPDIR"]
    ) as directory:
        work = Path(directory)
        source = work / "boot-completion.c"
        source.write_text(BOOT_COMPLETION)
        obj, elf, binary = (
            work / name for name in ("fixture.o", "fixture.elf", "fixture.bin")
        )
        subprocess.run(
            [
                clang,
                "--target=arm-none-eabi",
                "-mcpu=cortex-m0",
                "-mthumb",
                "-std=c11",
                "-Oz",
                "-ffreestanding",
                "-fno-builtin",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-Werror",
                "-I",
                str(ROOT / "tools/patches"),
                "-c",
                str(source),
                "-o",
                str(obj),
            ],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            [
                linker,
                f"-Ttext={FIXTURE_ADDRESS:#x}",
                "--entry=legacy_complete_boot",
                str(obj),
                "-o",
                str(elf),
            ],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            [objcopy, "-O", "binary", "--only-section=.text", str(elf), str(binary)],
            check=True,
            capture_output=True,
        )
        return binary.read_bytes()


class BootFixture:
    """Add the checked 8 KiB profile and a completed real trim boot lifetime."""

    trim_symbols = {}
    fixture_binary = b""
    boot_count = 0

    def __init__(self, *args, **kwargs):
        """Initialize production state before exposing the legacy test fixture."""
        super().__init__(*args, **kwargs)
        saved = {
            register: self.uc.reg_read(register)
            for register in (*child.REGS, UC_ARM_REG_XPSR, UC_ARM_REG_PRIMASK)
        }
        self.uc.mem_map(child.RAM + 0x1000, 0x1000)
        self.uc.mem_map(FIXTURE_ADDRESS, 0x1000)
        self.uc.mem_write(FIXTURE_ADDRESS, self.fixture_binary)
        for address in (0x40021000, 0x40015000, 0x1FFFF000):
            self.uc.mem_map(address, 0x1000)
        for address, value in (
            (0x40021018, 0x00400000),
            (0x40015800, 0x440),
            (0x1FFFF7CC, 64),
        ):
            self.uc.mem_write(address, value.to_bytes(4, "little"))
        self.uc.reg_write(UC_ARM_REG_PRIMASK, 1)
        self.call(self.trim_symbols["platform_bootstrap"])
        self.call(self.trim_symbols["tb01_init"])
        self.uc.reg_write(UC_ARM_REG_PRIMASK, 0)
        self.call(self.trim_symbols["trim_controller_boot"])
        child.require(
            self.call(self.trim_symbols["trim_controller_started"]) == 1,
            "real trim boot did not activate",
        )
        child.require(
            self.call(self.trim_symbols["trim_controller_blocks_native"]) == 1,
            "real boot ownership did not exclude native callers",
        )
        self.uc.reg_write(UC_ARM_REG_LR, child.RETURN | 1)
        self.stops, self.stopped_at = {child.RETURN}, None
        self.uc.emu_start(FIXTURE_ADDRESS | 1, 0, count=10000)
        child.require(
            self.stopped_at == child.RETURN and self.uc.reg_read(child.REGS[0]) == 1,
            "typed boot-completion fixture rejected real adapter state",
        )
        self.call(self.trim_symbols["trim_controller_poll"])
        child.require(
            self.call(self.trim_symbols["trim_controller_blocks_native"]) == 0,
            "real controller poll did not finish Default boot handoff",
        )
        child.require(
            self.call(self.trim_symbols["trim_controller_inputs_blocked"]) == 0,
            "released boot keys did not open the real input gate",
        )
        child.require(self.get(0x1050, 4) == 0, "Default boot published nonzero trims")
        for register, value in saved.items():
            self.uc.reg_write(register, value)
        type(self).boot_count += 1

    def _code(self, uc, address, size, data):
        if FIXTURE_ADDRESS <= address < FIXTURE_ADDRESS + len(self.fixture_binary):
            self.min_sp = min(self.min_sp, uc.reg_read(UC_ARM_REG_SP))
            self.visited.add(address)
            return
        super()._code(uc, address, size, data)

    def _write(self, uc, access, address, size, value, data):
        if child.RAM + 0x1000 <= address and address + size <= child.RAM + 0x2000:
            self.writes.append((address, size, value, uc.reg_read(UC_ARM_REG_PC)))
            return
        super()._write(uc, access, address, size, value, data)


def run(
    source: bytes, metadata: dict, llvm_bin: Path | None, selected: list[str]
) -> dict:
    """Bind the assembled image and run independent unchanged legacy suites."""
    child.require(unicorn_version == "2.1.4", "Unicorn 2.1.4 required")
    child.require(
        digest(source) == metadata["output_sha256"], "R7 manifest hash mismatch"
    )
    image, manifest = assemble(
        source, boot_profile=SYNTHETIC_BOOT_PROFILE, llvm_bin=llvm_bin
    )
    fixture = compile_fixture(llvm_bin)

    class LegacyMachine(BootFixture, child.Machine):
        """Execute parser/CL01/DT02 paths after real linked trim boot setup."""

    class LegacyResetMachine(BootFixture, reset.ResetMachine):
        """Execute original RS01 regression paths after real linked trim boot setup."""

    for cls in (LegacyMachine, LegacyResetMachine):
        cls.trim_symbols, cls.fixture_binary = manifest["symbols"], fixture
    rs_symbols = {
        name: int(value, 0) if isinstance(value, str) else value
        for name, value in metadata["symbols"].items()
    }
    checks, failures, parser_controls = {}, {}, []
    for name in selected:
        try:
            if name == "rs01":
                with patch.object(reset, "ResetMachine", LegacyResetMachine):
                    checks[name] = reset.run(image, rs_symbols)
            elif name == "parser":
                with patch.object(parser_boundary, "Machine", LegacyMachine):
                    for command in (0xD7, 0xD6):
                        for payload in (
                            bytes((0xFA, 0xFD, 0xFE)),
                            bytes((command ^ 0xFD,)),
                        ):
                            body = bytes((command,)) + payload
                            wire = child.frame(body)
                            probe = parser_boundary.Fixture(image, wire)
                            probe.expose(len(wire))
                            event = probe.call()
                            child.require(
                                event["accepted"] == len(body) + 1,
                                "unknown-command control parse length",
                            )
                            child.require(
                                event["up_stores"] == 0,
                                "unknown-command control moved UP",
                            )
                            child.require(
                                manifest["symbols"]["tb01_extension_route"]
                                in probe.machine.visited,
                                "unknown-command control skipped real TB01 route",
                            )
                            if command == 0xD7:
                                child.require(
                                    not event["reply"],
                                    "malformed TB01 command unexpectedly replied",
                                )
                            else:
                                child.require(
                                    child.decode(event["reply"])[:2]
                                    == bytes((0xEE, 3)),
                                    "D6 is not an unassigned-command control",
                                )
                            parser_controls.append(
                                {
                                    "body_hex": body.hex(),
                                    "reply_hex": event["reply"].hex(),
                                    "accepted": event["accepted"],
                                    "real_tb01_route": True,
                                }
                            )
                    cases = parser_boundary.run(image, unknown_command=0xD6)
                    checks[name] = {"passed": True, "count": len(cases), "cases": cases}
            else:
                sections, symbols = child_builder.compile_patch(llvm_bin)
                expected = sections["child_lock"]
                child.require(
                    image[0x8C00 : 0x8C00 + len(expected)] == expected,
                    "CL01 section differs from its recovered symbols",
                )
                with patch.object(child, "Machine", LegacyMachine):
                    suite = child.Suite(source, image, symbols)
                    method = "lock_protocol" if name == "cl01" else "direct_target"
                    checks[name] = {"passed": True, "cases": getattr(suite, method)()}
        except Exception as error:
            failures[name] = {
                "error": str(error),
                "traceback": traceback.format_exc(limit=5),
            }
    return {
        "passed": not failures,
        "hardware_tested": False,
        "analysis_image": manifest,
        "legacy_sources": {
            name: digest((ROOT / "tools" / name).read_bytes())
            for name in (
                "test_motion_trim_legacy.py",
                "test_reset_speed_patch.py",
                "test_uart_parser_boundary.py",
                "test_child_lock_patch.py",
            )
        },
        "fixture_source_sha256": digest(BOOT_COMPLETION.encode()),
        "fixture_binary_sha256": digest(fixture),
        "fixture_mapped_address": hex(FIXTURE_ADDRESS),
        "real_boot_lifetimes": LegacyMachine.boot_count + LegacyResetMachine.boot_count,
        "checks": checks,
        "failures": failures,
        "parser_fixture_controls": parser_controls,
        "parser_fixture_compatibility": (
            "R7's default unknown kind D7 is TB01 in the full shared image and consumes malformed lengths. "
            "The unchanged parser assertions use D6, dynamically verified to return native EE/03. "
            "The checksum payload is adjusted to retain the original escaped-FD checksum property."
        ),
        "limitations": [
            "The extra mapped flash page contains only the typed boot-completion fixture; the assembled image bytes are unchanged.",
            "EEPROM boot transport is replaced by a successful coherent Default completion certificate before real controller_poll.",
            "Legacy EEPROM, physical sampling and main-loop fixture boundaries remain those of the unchanged existing runners.",
            "No I2C electrical timing, motor physics, hardware initialization or whole-program stack bound is established.",
        ],
    }


def main() -> None:
    """Run selected legacy regressions and save hash-bound failures or results."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--llvm-bin", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument(
        "--only",
        nargs="+",
        choices=("rs01", "parser", "cl01", "dt02"),
        default=["rs01", "parser", "cl01", "dt02"],
    )
    args = parser.parse_args()
    report = run(
        args.source.read_bytes(),
        json.loads(args.manifest.read_text()),
        args.llvm_bin,
        args.only,
    )
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps(
            {
                "passed": report["passed"],
                "suites": list(report["checks"]),
                "failures": report["failures"],
                "report": str(args.report),
            },
            indent=2,
        )
    )
    raise SystemExit(0 if report["passed"] else 1)


if __name__ == "__main__":
    main()
